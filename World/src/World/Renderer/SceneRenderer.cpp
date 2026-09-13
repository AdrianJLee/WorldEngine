#include "wldpch.h"
#include "SceneRenderer.h"

#include "World/Renderer/Renderer.h"
#include "World/Renderer/Renderer2D.h"
#include "World/RHI/RhiTextureBridge.h"

#include <glm/gtc/matrix_transform.hpp>

namespace World
{
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
		m_CommandBuffer = m_Device->CreateCommandBuffer("SceneRenderer");

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
		m_CameraBuffer = m_Device->CreateBuffer(cameraDesc);

		m_GlobalDescriptorSet = m_Device->CreateDescriptorSet(
			Renderer::GetGlobalDescriptorSetLayout());
		Rhi::DescriptorWrite cameraWrite;
		cameraWrite.Binding = 0;
		cameraWrite.Type = Rhi::DescriptorType::UniformBuffer;
		cameraWrite.Buffer = m_CameraBuffer;
		m_GlobalDescriptorSet->Update({ cameraWrite });

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
		m_CommandBuffer = nullptr;
		m_Device = nullptr;
	}

	void SceneRenderer::RecreateTargets(uint32_t width, uint32_t height)
	{
		m_Width = std::max(1u, width);
		m_Height = std::max(1u, height);

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

		const glm::mat4 viewProjection = camera.GetProjectionMatrix() * glm::inverse(cameraTransform);
		m_CameraBuffer->SetData(&viewProjection, sizeof(glm::mat4));

		std::vector<Rhi::ClearValue> clears(3);
		clears[0].Color = { 0.1f, 0.1f, 0.1f, 1.0f };
		const int minusOne = -1;
		std::memcpy(&clears[1].Color, &minusOne, sizeof(int));
		clears[2].IsDepthStencil = true;
		clears[2].DepthStencil.Depth = 1.0f;

		m_CommandBuffer->Begin();
		m_CommandBuffer->BeginRenderPass(m_RenderPass, m_Framebuffer, clears);
		m_CommandBuffer->SetViewport({ 0, 0, static_cast<float>(m_Width), static_cast<float>(m_Height) });
		m_CommandBuffer->BindDescriptorSet(m_GlobalDescriptorSet);

		Renderer2D::StartBatch();
		Renderer2D::BeginScene(camera, cameraTransform, m_CommandBuffer);

		if (selectedEntity)
		{
			auto& transform = selectedEntity.GetComponent<TransformComponent>();
			Renderer2D::DrawRectCore(transform, { 1.0f, 0.5f, 0.0f, 1.0f }, selectedEntity);
			RenderDebug(camera, cameraTransform);
		}
		RenderGeometry(camera, cameraTransform);

		Renderer2D::EndScene();
		m_CommandBuffer->EndRenderPass();
		m_CommandBuffer->End();
		Renderer::SubmitScene(m_CommandBuffer, m_ColorTexture);
	}

	void SceneRenderer::RenderGeometry(const Camera&, const glm::mat4&)
	{
		{
			auto group = m_ActiveScene->m_Registry.group<TransformComponent>(entt::get<SpriteComponent>);
			for (auto entity : group)
			{
				auto [transform, sprite] = group.get<TransformComponent, SpriteComponent>(entity);
				Renderer2D::DrawQuadCore(transform.Transform, sprite.Texture, sprite.Color, nullptr,
					sprite.TilingFactor, static_cast<uint32_t>(entity));
			}
		}
		{
			auto view = m_ActiveScene->m_Registry.view<TransformComponent, CircleRendererComponent>();
			for (auto [entity, transform, circle] : view.each())
				Renderer2D::DrawCircleCore(transform.Transform, circle.Color, circle.Thickness,
					circle.Fade, static_cast<uint32_t>(entity));
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
