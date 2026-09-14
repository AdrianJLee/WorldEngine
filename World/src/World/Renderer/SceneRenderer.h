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
		Rhi::Handle<Rhi::Texture> GetColorTexture() const { return m_ColorTexture; }
		Rhi::Handle<Rhi::Framebuffer> GetRhiTarget() const { return m_Framebuffer; }
		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
		// 开发验证:把颜色附件读回写 PPM。
		void CaptureFrame(const std::filesystem::path& path) const;

	private:
		void RecreateTargets(uint32_t width, uint32_t height);
		void RecordSubmit(const Camera& camera, const glm::mat4& cameraTransform, Entity selectedEntity);
		void RenderGeometry(const Camera& camera, const glm::mat4& cameraTransform);
		void RenderDebug(const Camera& camera, const glm::mat4& cameraTransform);

	private:
		Rhi::Handle<Rhi::Device> m_Device;
		// 帧深 2:命令缓冲/相机 UBO/描述符集按帧槽位环形,允许 CPU 录制与 GPU 执行重叠。
		static constexpr uint32_t kFramesInFlight = 2;
		Rhi::Handle<Rhi::CommandBuffer> m_CommandBuffers[kFramesInFlight];
		Rhi::Handle<Rhi::RenderPass> m_RenderPass;
		Rhi::Handle<Rhi::Framebuffer> m_Framebuffer;
		Rhi::Handle<Rhi::Texture> m_ColorTexture;
		Rhi::Handle<Rhi::Texture> m_EntityTexture;
		Rhi::Handle<Rhi::Texture> m_DepthTexture;
		Rhi::Handle<Rhi::Buffer> m_CameraBuffers[kFramesInFlight];
		Rhi::Handle<Rhi::DescriptorSet> m_GlobalDescriptorSets[kFramesInFlight];

		uint32_t FrameSlot() const;

		uint32_t m_Width = 1280;
		uint32_t m_Height = 720;
		Scene* m_ActiveScene = nullptr;
		SceneRendererOptions m_Options;
		Ref<Framebuffer> m_FramebufferView;
	};
}
