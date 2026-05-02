#include "wldpch.h"
#include "SceneRenderer.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Scene/Components.h"
#include "World/Renderer/RenderCommand.h"

#include <glm/gtc/matrix_transform.hpp>
namespace World
{

	void SceneRenderer::Init()
	{
		// Create Framebuffer
		FramebufferSpecification fbSpec;
		fbSpec.Attachments = { FramebufferTextureFormat::RGBA8,FramebufferTextureFormat::RED_INTEGER, FramebufferTextureFormat::Depth };
		fbSpec.Width = 1280;
		fbSpec.Height = 720;
		m_MainFramebuffer = Framebuffer::Create(fbSpec);

		// Create Render Pass
		RenderPassSpecification passSpec;
		passSpec.TargetFramebuffer = m_MainFramebuffer;
		passSpec.ClearColor = { 0.1f, 0.1f, 0.1f, 1.0f };
		m_ActivePass = RenderPass::Create(passSpec);

		// Create Command Buffer
		m_CommandBuffer = CommandBuffer::Create();

		// Create Descriptor Set
		m_GlobalDescriptorSet = CreateRef<DescriptorSet>();
		// Binding 0: Camera UBO
		m_GlobalDescriptorSet->AddUniformBufferSet(DescriptorBindings::UniformBuffers::Pass::Camera, sizeof(glm::mat4), RenderCommand::GetMaxFramesInFlight());

	}

	void SceneRenderer::Shutdown()
	{
		WLD_PROFILE_FUNCTION();

		m_ActivePass = nullptr;

		if (m_MainFramebuffer)
		{
			m_MainFramebuffer->Unbind();
			m_MainFramebuffer = nullptr;
		}

		m_CommandBuffer = nullptr;

		m_ActiveScene = nullptr;
	}

	void SceneRenderer::BeginScene(const Scene* scene, const SceneRendererOptions& options)
	{
		m_ActiveScene = scene;
		m_Options = options;

		m_CommandBuffer->Begin(m_CurrentFrameIndex);
	}

	void SceneRenderer::SubmitScene(const Camera& camera, const glm::mat4& cameraTransform, Entity selectedEntity)
	{
		glm::mat4 uCameraData = camera.GetProjectionMatrix() * glm::inverse(cameraTransform);

		m_CommandBuffer->AddCommand([this, uCameraData]()
			{
				m_GlobalDescriptorSet->GetUniformBufferSet(DescriptorBindings::UniformBuffers::Pass::Camera)->SetData(&uCameraData, sizeof(glm::mat4));
			});

		m_CommandBuffer->BeginRenderPass(m_ActivePass, true);

		//m_CommandBuffer->StartBatch();
		Renderer2D::StartBatch();
		m_CommandBuffer->BindDescriptorSet(m_GlobalDescriptorSet);

		if (selectedEntity)
		{
			auto& transform = selectedEntity.GetComponent<TransformComponent>();
			Renderer2D::StartBatch();
			Renderer2D::BeginScene(camera, cameraTransform, m_CommandBuffer);
			Renderer2D::DrawRectCore(transform, { 1.0f, 0.5f, 0.0f, 1.0f }, selectedEntity);
			Renderer2D::EndScene();
		}

		RenderDebug(m_CommandBuffer, camera, cameraTransform);

		RenderGeometry(m_CommandBuffer, camera, cameraTransform);


		m_CommandBuffer->EndRenderPass();
	}

	void SceneRenderer::RenderGeometry(Ref<CommandBuffer> cmd, const Camera& camera, const glm::mat4& cameraTransform)
	{
		Renderer2D::BeginScene(camera, cameraTransform, cmd);

		{
			auto view = m_ActiveScene->m_Registry.view<SpriteComponent>();
			for (auto entity : view)
			{
				auto& transform = m_ActiveScene->m_Registry.get<TransformComponent>(entity);
				auto& sprite = view.get<SpriteComponent>(entity);
				Renderer2D::DrawQuadCore(transform, sprite.Texture, sprite.Color, nullptr, sprite.TilingFactor, (uint32_t)entity);

			}
		}


		{
			auto view = m_ActiveScene->m_Registry.view<CircleRendererComponent>();
			for (auto entity : view)
			{
				auto& transform = m_ActiveScene->m_Registry.get<TransformComponent>(entity);
				auto& circle = view.get<CircleRendererComponent>(entity);
				Renderer2D::DrawCircleCore(transform.Transform, circle.Color, circle.Thickness, circle.Fade, (uint32_t)entity);
			}
		}

		Renderer2D::EndScene();
	}

	void SceneRenderer::RenderDebug(Ref<CommandBuffer> cmd, const Camera& camera, const glm::mat4& cameraTransform)
	{
		Renderer2D::BeginScene(camera, cameraTransform, cmd);
		{
			auto view = m_ActiveScene->m_Registry.view<CircleCollider2DComponent>();
			for (auto entity : view)
			{
				auto& transform = m_ActiveScene->m_Registry.get<TransformComponent>(entity);
				auto& circleCollider = view.get<CircleCollider2DComponent>(entity);
				if (circleCollider.ShowCollider)
				{
					glm::vec4 color = { 0.1f, 0.9f, 0.1f, 1.0f };
					glm::mat4 colliderTransform = glm::translate(glm::mat4(1.0f), transform.Location) *
						glm::rotate(glm::mat4(1.0f), transform.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f)) *
						glm::translate(glm::mat4(1.0f), glm::vec3(circleCollider.Offset.x, circleCollider.Offset.y, 0.0f)) *
						glm::scale(glm::mat4(1.0f), glm::vec3(transform.Scale.x * circleCollider.Radius * 2.0f, transform.Scale.y * circleCollider.Radius * 2.0f, 1.0f));
					Renderer2D::DrawCircleCore(colliderTransform, color, 0.025f, 0.005f, (uint32_t)entity);
				}
			}
		}

		{
			auto view = m_ActiveScene->m_Registry.view<BoxCollider2DComponent>();
			for (auto entity : view)
			{
				auto& transform = m_ActiveScene->m_Registry.get<TransformComponent>(entity);
				auto& boxCollider = view.get<BoxCollider2DComponent>(entity);
				if (boxCollider.ShowCollider)
				{
					glm::vec4 color = { 0.1f, 0.9f, 0.1f, 1.0f };
					glm::vec3 scale = { transform.Scale.x * boxCollider.Size.x * 2, transform.Scale.y * boxCollider.Size.y * 2, 1.0f };
					glm::mat4 colliderTransform = glm::translate(glm::mat4(1.0f), transform.Location) *
						glm::rotate(glm::mat4(1.0f), transform.Rotation.z, glm::vec3(0.0f, 0.0f, 1.0f)) *
						glm::translate(glm::mat4(1.0f), glm::vec3(boxCollider.Offset.x, boxCollider.Offset.y, 0.0f)) *
						glm::scale(glm::mat4(1.0f), scale);

					Renderer2D::DrawRectCore(colliderTransform, color, (uint32_t)entity);
				}
			}

		}
		Renderer2D::EndScene();
	}

	void SceneRenderer::EndScene()
	{
		m_CommandBuffer->End();

		m_CommandBuffer->Execute();

		m_ActiveScene = nullptr;

		m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % RenderCommand::GetMaxFramesInFlight();
	}

	void SceneRenderer::OnResize(uint32_t width, uint32_t height)
	{
		m_MainFramebuffer->Resize(width, height);
	}
}