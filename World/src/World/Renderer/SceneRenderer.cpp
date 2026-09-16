#include "wldpch.h"
#include "SceneRenderer.h"

#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/Mesh.h"
#include "World/Renderer/ProjectionConventions.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/RHI/RhiTextureBridge.h"
#include "World/Core/Thread/JobSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>

namespace World
{
	uint32_t SceneRenderer::FrameSlot() const
	{
		return static_cast<uint32_t>(Renderer::FrameSlot());
	}

	namespace
	{
		// 旧 Framebuffer 接口适配器:编辑器显示/拾取仍走 GL id,迁到 RHI 后移除。
		class RhiFramebufferAdapter final : public Framebuffer
		{
		public:
			SceneRenderer* Owner = nullptr;
			Rhi::Handle<Rhi::Framebuffer> Target;
			uint32_t Width = 0;
			uint32_t Height = 0;

			void Resize(uint32_t width, uint32_t height) override
			{
				if (Owner)
					Owner->OnResize(width, height);
			}

			const FramebufferSpecification& GetSpecification() const override
			{
				static FramebufferSpecification spec;
				return spec;
			}

			uint32_t GetColorAttachmentRendererID(size_t index = 0) const override
			{
				return Rhi::FramebufferAttachmentId(Target, index);
			}

			void Bind() override {}
			void Unbind() override {}

			int ReadPixel(uint32_t attachmentIndex, int x, int y) override
			{
				return Rhi::FramebufferReadPixel(Target, attachmentIndex, x, y);
			}

			void ClearAttachment(uint32_t, int) override {}
		};
	}

	void SceneRenderer::Init()
	{
		WLD_PROFILE_FUNCTION();
		if (m_Device)
			Shutdown();
		m_Device = Renderer::GetDevice();
		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
			m_CommandBuffers[slot] = m_Device->CreateCommandBuffer("SceneRenderer");

		Rhi::RenderPassDesc passDesc;
		Rhi::RenderPassAttachment color;
		color.Format = Rhi::Format::R8G8B8A8_UNORM;
		color.Samples = Rhi::SampleCount::Count1;
		color.Load = Rhi::LoadOp::Clear;
		color.Store = Rhi::StoreOp::Store;
		color.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		color.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		color.Clear.Color = { 0.1f, 0.1f, 0.1f, 1.0f };

		Rhi::RenderPassAttachment entityId;
		entityId.Format = Rhi::Format::R32_SINT;
		entityId.Samples = Rhi::SampleCount::Count1;
		entityId.Load = Rhi::LoadOp::Clear;
		entityId.Store = Rhi::StoreOp::Store;
		entityId.InitialLayout = Rhi::AttachmentLayout::ColorAttachment;
		entityId.FinalLayout = Rhi::AttachmentLayout::ColorAttachment;
		const int minusOne = -1;
		std::memcpy(&entityId.Clear.Color, &minusOne, sizeof(int));

		Rhi::RenderPassAttachment depth;
		depth.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depth.Samples = Rhi::SampleCount::Count1;
		depth.Load = Rhi::LoadOp::Clear;
		depth.Store = Rhi::StoreOp::Store;
		depth.InitialLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		depth.FinalLayout = Rhi::AttachmentLayout::DepthStencilAttachment;
		depth.Clear.IsDepthStencil = true;
		depth.Clear.DepthStencil.Depth = 1.0f;

		passDesc.Attachments = { color, entityId, depth };
		Rhi::SubpassDesc subpass;
		subpass.ColorAttachments = {
			{ 0, Rhi::AttachmentLayout::ColorAttachment },
			{ 1, Rhi::AttachmentLayout::ColorAttachment },
		};
		subpass.DepthStencilAttachment = { 2, Rhi::AttachmentLayout::DepthStencilAttachment };
		passDesc.Subpasses = { subpass };
		m_RenderPass = m_Device->CreateRenderPass(passDesc);

		Rhi::BufferDesc cameraDesc;
		cameraDesc.Size = sizeof(glm::mat4);
		cameraDesc.Usage = Rhi::BufferUsageUniform;
		cameraDesc.Memory = Rhi::MemoryHint::HostVisible;
		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
		{
			m_CameraBuffers[slot] = m_Device->CreateBuffer(cameraDesc);
			m_GlobalDescriptorSets[slot] = m_Device->CreateDescriptorSet(
				Renderer::GetGlobalDescriptorSetLayout());
			Rhi::DescriptorWrite cameraWrite;
			cameraWrite.Binding = 0;
			cameraWrite.Type = Rhi::DescriptorType::UniformBuffer;
			cameraWrite.Buffer = m_CameraBuffers[slot];
			m_GlobalDescriptorSets[slot]->Update({ cameraWrite });
		}

		m_FramebufferView = CreateRef<RhiFramebufferAdapter>();
		static_cast<RhiFramebufferAdapter*>(m_FramebufferView.get())->Owner = this;
		RecreateTargets(m_Width, m_Height);
	}

	void SceneRenderer::Shutdown()
	{
		WLD_PROFILE_FUNCTION();
		m_ActiveScene = nullptr;
		m_Framebuffer = nullptr;
		m_FramebufferView = nullptr;
		m_ColorTexture = nullptr;
		m_EntityTexture = nullptr;
		m_DepthTexture = nullptr;
		m_RenderPass = nullptr;
		for (uint32_t slot = 0; slot < kFramesInFlight; ++slot)
		{
			m_CommandBuffers[slot] = nullptr;
			m_CameraBuffers[slot] = nullptr;
			m_GlobalDescriptorSets[slot] = nullptr;
		}
		m_FramebufferView = nullptr;
		m_Device = nullptr;
	}

	void SceneRenderer::RecreateTargets(uint32_t width, uint32_t height)
	{
		m_Width = std::max(1u, width);
		m_Height = std::max(1u, height);
		// 旧目标可能仍被在飞的帧引用:交给延迟释放队列,在栅栏通过后回收。
		if (m_Framebuffer || m_ColorTexture || m_EntityTexture || m_DepthTexture)
		{
			auto oldFramebuffer = m_Framebuffer;
			auto oldColor = m_ColorTexture;
			auto oldEntity = m_EntityTexture;
			auto oldDepth = m_DepthTexture;
			Renderer::QueueRelease([oldFramebuffer, oldColor, oldEntity, oldDepth]() {});
		}

		Rhi::TextureDesc colorDesc;
		colorDesc.Type = Rhi::TextureType::Texture2D;
		colorDesc.Format = Rhi::Format::R8G8B8A8_UNORM;
		colorDesc.Extent = { m_Width, m_Height, 1 };
		colorDesc.Usage = Rhi::TextureUsageColorAttachment | Rhi::TextureUsageSampled;
		m_ColorTexture = m_Device->CreateTexture(colorDesc);

		Rhi::TextureDesc entityDesc;
		entityDesc.Type = Rhi::TextureType::Texture2D;
		entityDesc.Format = Rhi::Format::R32_SINT;
		entityDesc.Extent = { m_Width, m_Height, 1 };
		entityDesc.Usage = Rhi::TextureUsageColorAttachment;
		m_EntityTexture = m_Device->CreateTexture(entityDesc);

		Rhi::TextureDesc depthDesc;
		depthDesc.Type = Rhi::TextureType::Texture2D;
		depthDesc.Format = Rhi::Format::D24_UNORM_S8_UINT;
		depthDesc.Extent = { m_Width, m_Height, 1 };
		depthDesc.Usage = Rhi::TextureUsageDepthStencilAttachment;
		m_DepthTexture = m_Device->CreateTexture(depthDesc);

		Rhi::FramebufferDesc framebufferDesc;
		framebufferDesc.RenderPass = m_RenderPass;
		framebufferDesc.Extent = { m_Width, m_Height };
		framebufferDesc.Attachments = { m_ColorTexture, m_EntityTexture, m_DepthTexture };
		m_Framebuffer = m_Device->CreateFramebuffer(framebufferDesc);

		if (m_FramebufferView)
		{
			auto* adapter = static_cast<RhiFramebufferAdapter*>(m_FramebufferView.get());
			adapter->Target = m_Framebuffer;
			adapter->Width = m_Width;
			adapter->Height = m_Height;
		}
	}

	void SceneRenderer::BeginScene(Scene* scene, const SceneRendererOptions& options)
	{
		m_ActiveScene = scene;
		m_Options = options;
	}

	void SceneRenderer::EndScene()
	{
		m_ActiveScene = nullptr;
	}

	void SceneRenderer::SubmitScene(const Camera& camera, const glm::mat4& cameraTransform)
	{
		RecordSubmit(camera, cameraTransform, Entity());
	}

	void SceneRenderer::SubmitScene(const Camera& camera, const glm::mat4& cameraTransform, Entity entity)
	{
		RecordSubmit(camera, cameraTransform, entity);
	}

	void SceneRenderer::RecordSubmit(const Camera& camera, const glm::mat4& cameraTransform, Entity selectedEntity)
	{
		if (!m_ActiveScene)
			return;

		glm::mat4 viewProjection = camera.GetProjectionMatrix() * glm::inverse(cameraTransform);
		// 编辑/运行期都会改 Transform:每帧先重算层级世界矩阵,子实体才会跟随父实体
		// (此前只有序列化/Prefab 路径求解,见 Hierarchy.h)。
		Hierarchy::UpdateWorldTransforms(m_ActiveScene->m_Registry);
		// 后端适配:场景渲染到**离屏纹理**(WUI 用固定 UV 贴到视口),
		// 因此 Vulkan 只补深度范围、**不翻 Y**(翻了会在视口里上下颠倒,实测)。
		viewProjection = AdaptViewProjectionForOffscreen(viewProjection, Renderer::GetBackendName() == "vulkan");
		const uint32_t slot = FrameSlot();
		m_CameraBuffers[slot]->SetData(&viewProjection, sizeof(glm::mat4));

		std::vector<Rhi::ClearValue> clears(3);
		clears[0].Color = { 0.1f, 0.1f, 0.1f, 1.0f };
		const int minusOne = -1;
		std::memcpy(&clears[1].Color, &minusOne, sizeof(int));
		clears[2].IsDepthStencil = true;
		clears[2].DepthStencil.Depth = 1.0f;

		m_CommandBuffers[slot]->Begin();
		m_CommandBuffers[slot]->BeginRenderPass(m_RenderPass, m_Framebuffer, clears);
		m_CommandBuffers[slot]->SetViewport({ 0, 0, static_cast<float>(m_Width), static_cast<float>(m_Height) });
		// 管线把视口/裁剪都设为动态状态,绑定后必须先设置再绘制
		// (VUID-vkCmdDrawIndexed-None-07832:动态裁剪未设置时状态未定义)。
		m_CommandBuffers[slot]->SetScissor({ 0, 0, m_Width, m_Height });
		m_CommandBuffers[slot]->BindDescriptorSet(m_GlobalDescriptorSets[slot]);

		// 开发钩子:WLD_DEBUG_CUBE=1 时在同一渲染通道里提交一个 3D 立方体,
		// 作为 3D 通道(Pipeline/深度/网格缓冲)在双后端下的冒烟基线(D2b)。
		if (std::getenv("WLD_DEBUG_CUBE"))
		{
			if (!m_DebugCube)
				m_DebugCube = Mesh::CreateUnitCube(1.0f);
			if (m_DebugCube)
			{
				Renderer3D::BeginScene(viewProjection, m_CommandBuffers[slot]);
				// 放在画面右上方,避免与 2D 精灵基线区域重叠(便于像素校验)。
				const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(0.78f, 0.62f, 0.0f))
					* glm::rotate(glm::mat4(1.0f), glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f))
					* glm::rotate(glm::mat4(1.0f), glm::radians(22.0f), glm::vec3(1.0f, 0.0f, 0.0f))
					* glm::scale(glm::mat4(1.0f), glm::vec3(0.42f));
				Renderer3D::Submit(m_DebugCube, model, { 1.0f, 0.55f, 0.12f, 1.0f });
				Renderer3D::EndScene();
			}
		}

		// 3D 网格通道(D2c):MeshRendererComponent 实体用 Transform 作模型矩阵、Color 作基色。
		// Sprite 组件保持纯 2D,不再参与 3D 提交。
		{
			auto meshView = m_ActiveScene->m_Registry.view<TransformComponent, MeshRendererComponent>();
			bool began = false;
			for (auto entity : meshView)
			{
				const auto& [transform, meshComponent] =
					meshView.get<TransformComponent, MeshRendererComponent>(entity);
				const bool plane = meshComponent.Primitive == "plane";
				Ref<Mesh>& mesh = plane ? m_DebugPlane : m_DebugCube;
				if (!mesh)
					mesh = plane ? Mesh::CreateUnitPlane(1.0f) : Mesh::CreateUnitCube(1.0f);
				if (!mesh)
					continue;

				if (!began)
				{
					Renderer3D::BeginScene(viewProjection, m_CommandBuffers[slot]);
					began = true;
				}
				// 层级实体用求解后的世界矩阵:直接提交本地矩阵会让子实体不跟随父实体
				// (实测"移动父项子项不动")。世界矩阵由本轮统一求解(见上方 UpdateWorldTransforms)。
				const glm::mat4* modelMatrix = &transform.Transform;
				if (m_ActiveScene && m_ActiveScene->m_Registry.all_of<WorldTransformComponent>(entity))
					modelMatrix = &m_ActiveScene->m_Registry.get<WorldTransformComponent>(entity).Matrix;
				// D7-1c:把实体 id 一起提交,写进 entity-id 附件供视口点选读回。
				Renderer3D::Submit(mesh, *modelMatrix, meshComponent.Color,
					static_cast<int32_t>(static_cast<uint32_t>(entity)));
			}
			if (began)
				Renderer3D::EndScene();
		}

		Renderer2D::StartBatch();
		Renderer2D::BeginScene(camera, cameraTransform, m_CommandBuffers[slot]);

		if (selectedEntity)
		{
			auto& transform = selectedEntity.GetComponent<TransformComponent>();
			Renderer2D::DrawRectCore(transform, { 1.0f, 0.5f, 0.0f, 1.0f }, selectedEntity);
			RenderDebug(camera, cameraTransform);
		}
		RenderGeometry(camera, cameraTransform);

		Renderer2D::EndScene();
		m_CommandBuffers[slot]->EndRenderPass();
		// 场景颜色附件在命令缓冲内转为可采样布局:提交方无需再 WaitIdle 做外部转换,
		// 同一队列上后续提交(UI)按顺序即可安全采样。
		{
			Rhi::ResourceBarrier barrier;
			barrier.Texture = m_ColorTexture;
			barrier.Before = Rhi::ResourceState::ColorAttachment;
			barrier.After = Rhi::ResourceState::ShaderReadOnly;
			m_CommandBuffers[slot]->PipelineBarrier({ barrier });
		}
		m_CommandBuffers[slot]->End();
		Renderer::SubmitScene(m_CommandBuffers[slot], m_ColorTexture);
	}

	void SceneRenderer::RenderGeometry(const Camera&, const glm::mat4&)
	{
		// B3:每实体的顶点变换(纯数学,不触碰批次状态机)按阈值并行;
		// 批次写入仍按原顺序在主线程执行,绘制顺序与像素结果与串行版本一致。
		constexpr size_t kParallelPrepThreshold = 64;
		{
			auto group = m_ActiveScene->m_Registry.group<TransformComponent>(entt::get<SpriteComponent>);
			std::vector<entt::entity> entities;
			for (auto entity : group)
				entities.push_back(entity);
			const size_t count = entities.size();
			if (count >= kParallelPrepThreshold && JobSystem::IsRunning())
			{
				std::vector<glm::mat4> transforms(count);
				std::vector<std::array<glm::vec3, 4>> positions(count);
				for (size_t i = 0; i < count; i++)
					transforms[i] = std::get<0>(
						group.get<TransformComponent, SpriteComponent>(entities[i])).Transform;

				JobSystem::ParallelFor(static_cast<uint32_t>(count), 32, [&](uint32_t i)
				{
					Renderer2D::ComputeQuadPositions(transforms[i], positions[i].data());
				});

				for (size_t i = 0; i < count; i++)
				{
					auto [transform, sprite] = group.get<TransformComponent, SpriteComponent>(entities[i]);
					Renderer2D::DrawQuadPositions(positions[i].data(), sprite.Texture, sprite.Color,
						nullptr, sprite.TilingFactor, static_cast<uint32_t>(entities[i]));
				}
			}
			else
			{
				for (auto entity : entities)
				{
					auto [transform, sprite] = group.get<TransformComponent, SpriteComponent>(entity);
					Renderer2D::DrawQuadCore(transform.Transform, sprite.Texture, sprite.Color, nullptr,
						sprite.TilingFactor, static_cast<uint32_t>(entity));
				}
			}
		}
		{
			auto view = m_ActiveScene->m_Registry.view<TransformComponent, CircleRendererComponent>();
			std::vector<entt::entity> entities;
			for (auto entity : view)
				entities.push_back(entity);
			const size_t count = entities.size();
			if (count >= kParallelPrepThreshold && JobSystem::IsRunning())
			{
				std::vector<glm::mat4> transforms(count);
				std::vector<std::array<glm::vec3, 4>> positions(count);
				for (size_t i = 0; i < count; i++)
					transforms[i] = view.get<TransformComponent>(entities[i]).Transform;
				JobSystem::ParallelFor(static_cast<uint32_t>(count), 32, [&](uint32_t i)
				{
					Renderer2D::ComputeCirclePositions(transforms[i], positions[i].data());
				});
				for (size_t i = 0; i < count; i++)
				{
					const auto& circle = view.get<CircleRendererComponent>(entities[i]);
					Renderer2D::DrawCirclePositions(positions[i].data(), circle.Color, circle.Thickness,
						circle.Fade, static_cast<uint32_t>(entities[i]));
				}
			}
			else
			{
				for (auto entity : entities)
				{
					auto& transform = view.get<TransformComponent>(entity);
					const auto& circle = view.get<CircleRendererComponent>(entity);
					Renderer2D::DrawCircleCore(transform.Transform, circle.Color, circle.Thickness,
						circle.Fade, static_cast<uint32_t>(entity));
				}
			}
		}
	}

	void SceneRenderer::RenderDebug(const Camera&, const glm::mat4&)
	{
		auto view = m_ActiveScene->m_Registry.view<CircleCollider2DComponent>();
		for (auto entity : view)
		{
			auto& transform = m_ActiveScene->m_Registry.get<TransformComponent>(entity);
			auto& circleCollider = view.get<CircleCollider2DComponent>(entity);
			if (!circleCollider.ShowCollider)
				continue;
			glm::mat4 colliderTransform = glm::translate(glm::mat4(1.0f), transform.Location)
				* glm::rotate(glm::mat4(1.0f), transform.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f))
				* glm::translate(glm::mat4(1.0f), glm::vec3(circleCollider.Offset.x, circleCollider.Offset.y, 0.0f))
				* glm::scale(glm::mat4(1.0f), glm::vec3(transform.Scale.x * circleCollider.Radius * 2.0f,
					transform.Scale.y * circleCollider.Radius * 2.0f, 1.0f));
			Renderer2D::DrawCircleCore(colliderTransform, { 0.1f, 0.9f, 0.1f, 1.0f },
				0.025f, 0.005f, static_cast<uint32_t>(entity));
		}
	}

	void SceneRenderer::OnResize(uint32_t width, uint32_t height)
	{
		RecreateTargets(width, height);
	}

	void SceneRenderer::CaptureFrame(const std::filesystem::path& path) const
	{
		// 走后端无关的 RHI 读回:GL 的延迟命令列表与 Vulkan 的呈现路径下,
		// 旧的 glReadPixels 版本分别只能抓到清屏色与全黑。
		Renderer::CaptureTexture(path, m_ColorTexture, m_Width, m_Height);
		// 诊断/验证(D7-1c):WLD_CAPTURE_ENTITY=<路径> 时把 entity-id 附件一起读回。
		// 附件是 R32_SINT:0xFFFFFFFF(-1)读成白色 = 该像素没有实体,其它值就是实体句柄。
		if (const char* entityPath = std::getenv("WLD_CAPTURE_ENTITY"))
			Renderer::CaptureTexture(entityPath, m_EntityTexture, m_Width, m_Height);
	}

	// D7-1c:视口点选的后端无关读回。entity-id 附件是 R32_SINT 颜色附件,一帧结束停在
	// ColorAttachment(见 Init() 里 RenderPassAttachment::FinalLayout)。这里复用截图那条
	// RHI 通路(CopyTextureToBuffer + Map):整张附件拷到 host-visible buffer 后取目标像素。
	// 旧实现走 Framebuffer::ReadPixel(glReadPixels),Vulkan 下读不到东西。
	// 点击是低频操作,因此接受一次 WaitIdle + 全附件拷贝的代价。
	int32_t SceneRenderer::ReadEntityIdAt(int32_t x, int32_t y)
	{
		if (!m_Device || !m_EntityTexture || m_Width == 0 || m_Height == 0)
			return -1;
		if (x < 0 || y < 0 || x >= static_cast<int32_t>(m_Width) || y >= static_cast<int32_t>(m_Height))
			return -1;

		Rhi::BufferDesc readbackDesc;
		readbackDesc.Size = static_cast<uint64_t>(m_Width) * m_Height * 4;
		readbackDesc.Usage = Rhi::BufferUsageTransferDst;
		readbackDesc.Memory = Rhi::MemoryHint::HostVisible;
		readbackDesc.DebugName = "PickReadback";
		Rhi::Handle<Rhi::Buffer> readback = m_Device->CreateBuffer(readbackDesc);
		if (!readback)
		{
			WLD_CORE_ERROR("[pick] failed to create readback buffer");
			return -1;
		}
		// 与截图同样复用队列:每次点选都建队列会泄漏后端对象。
		static Rhi::Handle<Rhi::CommandQueue> s_PickQueue;
		if (!s_PickQueue)
			s_PickQueue = m_Device->CreateQueue("Pick");
		if (!s_PickQueue)
			return -1;
		// 先等干净:保证读到的是点击前最后一次提交的附件内容。
		m_Device->WaitIdle();

		s_PickQueue->ExecuteImmediate([&](Rhi::CommandBuffer& cmd)
		{
			Rhi::ResourceBarrier toCopy;
			toCopy.Texture = m_EntityTexture;
			toCopy.Before = Rhi::ResourceState::ColorAttachment;
			toCopy.After = Rhi::ResourceState::CopySrc;
			cmd.PipelineBarrier({ toCopy });
			cmd.CopyTextureToBuffer(m_EntityTexture, readback, 0);
			Rhi::ResourceBarrier back;
			back.Texture = m_EntityTexture;
			back.Before = Rhi::ResourceState::CopySrc;
			back.After = Rhi::ResourceState::ColorAttachment;
			cmd.PipelineBarrier({ back });
		});

		const int32_t* pixels = static_cast<const int32_t*>(readback->Map());
		if (!pixels)
		{
			WLD_CORE_ERROR("[pick] readback buffer is not mappable on this backend");
			return -1;
		}
		// 行序:两个后端的读回都是"纹理第 0 行在前",而场景目标的第 0 行就是画面顶部
		// (离屏不做 Y 翻转,见 ProjectionConventions.h)。实测 GL/Vulkan 的 entity 附件
		// 读回逐行一致,因此这里**不做**任何翻转 —— 旧代码为 GL 翻一次行是错的。
		const size_t row = static_cast<size_t>(y);
		const int32_t id = pixels[row * m_Width + static_cast<uint32_t>(x)];
		if (std::getenv("WLD_TRACE_UI"))
		{
			// 临时诊断:确认读回缓冲里到底有什么(全 -1 = 拷到的是清屏值/空缓冲)。
			int32_t minValue = INT32_MAX, maxValue = INT32_MIN;
			size_t nonMinusOne = 0;
			int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = -1, maxY = -1;
			const size_t total = static_cast<size_t>(m_Width) * m_Height;
			for (size_t i = 0; i < total; ++i)
			{
				const int32_t value = pixels[i];
				minValue = std::min(minValue, value);
				maxValue = std::max(maxValue, value);
				if (value != -1)
				{
					++nonMinusOne;
					const int32_t px = static_cast<int32_t>(i % m_Width);
					const int32_t py = static_cast<int32_t>(i / m_Width);
					minX = std::min(minX, px); maxX = std::max(maxX, px);
					minY = std::min(minY, py); maxY = std::max(maxY, py);
				}
			}
			WLD_CORE_INFO("[pick] readback {0}x{1} min={2} max={3} hits={4} bbox=({5},{6})-({7},{8}) at({9},{10})row={11} id={12}",
				m_Width, m_Height, minValue, maxValue, nonMinusOne, minX, minY, maxX, maxY, x, y, row, id);
		}
		readback->Unmap();
		return id;
	}
}



