#pragma once
#pragma once
#include "World/Renderer/RenderPass.h"
#include "World/Renderer/CommandBuffer.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Scene/Scene.h"
#include "World/Renderer/DescriptorSet.h"
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


		// 核心：解析场景并决定渲染顺序
		void SubmitScene(const Camera& camera, const glm::mat4& cameraTransform, Entity entity);
		// 纯Game视图渲染，不考虑选中实体等编辑器特有功能
		void SubmitScene(const Camera& camera, const glm::mat4& cameraTransform);

		void OnResize(uint32_t width, uint32_t height);

		Ref<Framebuffer> GetTargetFramebuffer() const { return m_MainFramebuffer; }

	private:
		// 渲染分支辅助函数
		void RenderGeometry(Ref<CommandBuffer> cmd, const Camera& camera, const glm::mat4& cameraTransform);
		void RenderDebug(Ref<CommandBuffer> cmd, const Camera& camera, const glm::mat4& cameraTransform);

	private:
		Ref<Framebuffer> m_MainFramebuffer;
		Ref<RenderPass> m_ActivePass;
		Ref<CommandBuffer> m_CommandBuffer; // 当前帧的指令载体

		SceneRendererOptions m_Options;
		Scene* m_ActiveScene = nullptr;

		uint32_t m_CurrentFrameIndex = 0;

		Ref<DescriptorSet> m_GlobalDescriptorSet;
	};
}