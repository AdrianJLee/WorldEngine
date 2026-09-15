#include "wldpch.h"
#include "SceneRenderer.h"

#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Renderer/Mesh.h"
#include "World/Scene/Components.h"
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
		// 相机投影按"NDC +Y 向上"编写(GL 约定);Vulkan 的 NDC +Y 向下,
		// 翻转投影的 Y 行,场景在两种后端保持同一方向。
		if (Renderer::GetBackendName() == "vulkan")
		{
			viewProjection[0][1] = -viewProjection[0][1];
			viewProjection[1][1] = -viewProjection[1][1];
			viewProjection[2][1] = -viewProjection[2][1];
			viewProjection[3][1] = -viewProjection[3][1];
		}
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

		// 场景驱动的 3D 网格(临时约定,D2c 的 MeshRendererComponent 会取代):
		// Tag 以 "Mesh:Cube" / "Mesh:Plane" 开头的实体,用 Transform 作模型矩阵、Sprite.Color 作基色。
		// 这样 3D 关卡可以**只用现有组件**在编辑器里搭出来(拖 Transform、改颜色即可)。
		{
			auto meshView = m_ActiveScene->m_Registry.view<TransformComponent, SpriteComponent, TagComponent>();
			bool began = false;
			for (auto entity : meshView)
			{
				const auto& [transform, sprite, tag] =
					meshView.get<TransformComponent, SpriteComponent, TagComponent>(entity);
				const bool cube = tag.Tag.rfind("Mesh:Cube", 0) == 0;
				const bool plane = tag.Tag.rfind("Mesh:Plane", 0) == 0;
				if (!cube && !plane)
					continue;

				Ref<Mesh>& mesh = cube ? m_DebugCube : m_DebugPlane;
				if (!mesh)
					mesh = cube ? Mesh::CreateUnitCube(1.0f) : Mesh::CreateUnitPlane(1.0f);
				if (!mesh)
					continue;

				if (!began)
				{
					Renderer3D::BeginScene(viewProjection, m_CommandBuffers[slot]);
					began = true;
				}
				Renderer3D::Submit(mesh, transform.Transform, sprite.Color);
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
		Renderer::CaptureFramebuffer(path, Rhi::FramebufferId(m_Framebuffer), m_Width, m_Height);
	}
}



