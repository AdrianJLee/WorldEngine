#pragma once

#include "World/RHI/Rhi.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Renderer/Framebuffer.h"
#include "World/Scene/Scene.h"

#include <glm/glm.hpp>

namespace World
{
	struct SceneRendererOptions
	{
		bool ShowGrid = true;
	};

	class SceneRenderer
	{
	public:
		void Init();
		void Shutdown();

		void BeginScene(Scene* scene, const SceneRendererOptions& options);
		void EndScene();

		void SubmitScene(const Camera& camera, const glm::mat4& cameraTransform, Entity entity);
		void SubmitScene(const Camera& camera, const glm::mat4& cameraTransform);

		void OnResize(uint32_t width, uint32_t height);

		Ref<Framebuffer> GetTargetFramebuffer() const { return m_FramebufferView; }
		// 开发验证:把颜色附件读回写 PPM。
		void CaptureFrame(const std::filesystem::path& path) const;

	private:
		void RecreateTargets(uint32_t width, uint32_t height);
		void RecordSubmit(const Camera& camera, const glm::mat4& cameraTransform, Entity selectedEntity);
		void RenderGeometry(const Camera& camera, const glm::mat4& cameraTransform);
		void RenderDebug(const Camera& camera, const glm::mat4& cameraTransform);

	private:
		Rhi::Handle<Rhi::Device> m_Device;
		Rhi::Handle<Rhi::CommandBuffer> m_CommandBuffer;
		Rhi::Handle<Rhi::RenderPass> m_RenderPass;
		Rhi::Handle<Rhi::Framebuffer> m_Framebuffer;
		Rhi::Handle<Rhi::Texture> m_ColorTexture;
		Rhi::Handle<Rhi::Texture> m_EntityTexture;
		Rhi::Handle<Rhi::Texture> m_DepthTexture;
		Rhi::Handle<Rhi::Buffer> m_CameraBuffer;
		Rhi::Handle<Rhi::DescriptorSet> m_GlobalDescriptorSet;

		uint32_t m_Width = 1280;
		uint32_t m_Height = 720;
		Scene* m_ActiveScene = nullptr;
		SceneRendererOptions m_Options;
		Ref<Framebuffer> m_FramebufferView;
	};
}
