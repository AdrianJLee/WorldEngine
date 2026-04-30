#pragma once
#pragma once
#include "World/Renderer/RenderPass.h"
#include "World/Renderer/CommandBuffer.h"
#include "World/Renderer/EditorCamera.h"
#include "World/Scene/Scene.h"
#include <glm/glm.hpp>
namespace World
{
	struct SceneRendererOptions
	{
		bool ShowGrid = true;
		bool ShowPhysicsColliders = true;
	};

	class SceneRenderer
	{
	public:
		void Init();
		void Shutdown();

		void BeginScene(const Scene* scene, const SceneRendererOptions& options);
		void EndScene();

		// 核心：解析场景并决定渲染顺序
		void SubmitScene(const Camera& camera, const glm::mat4& cameraTransform, Entity entity);

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
		const Scene* m_ActiveScene = nullptr;
	};
}