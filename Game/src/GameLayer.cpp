#include "GameLayer.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/Renderer/RenderCommand.h"

namespace World
{
	GameLayer::GameLayer(WorldContext& context)
		: Layer("GameLayer"), m_Context(&context)
	{

	}
	void GameLayer::OnAttach()
	{
		WLD_PROFILE_FUNCTION();
		m_SceneRenderer = CreateRef<SceneRenderer>();

		m_SceneRenderer->Init();


		LoadScene();

	}
	void GameLayer::OnDetach()
	{
		WLD_PROFILE_FUNCTION();
		m_SceneRenderer->Shutdown();
	}
	void GameLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();
		if (m_ActiveScene)
		{
			Renderer2D::ResetStats();

			auto entity = m_ActiveScene->GetPrimaryCameraEntity();
			if (entity)
			{
				m_ActiveScene->OnUpdateRuntime(ts);
				auto& camera = entity.GetComponent<CameraComponent>().Camera;
				auto& transform = entity.GetComponent<TransformComponent>().Transform;

				m_SceneRenderer->BeginScene(m_ActiveScene.get(), SceneRendererOptions());
				m_SceneRenderer->SubmitScene(camera, transform);
				m_SceneRenderer->EndScene();

			}
			else
			{
				m_ActiveScene->OnUpdateRuntime(ts);
			}


		}
	}
	void GameLayer::OnImGuiRender()
	{

	}
	void GameLayer::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		// 拦截窗口 Resize 事件以动态更新相机投影矩阵
		dispatcher.Dispatch<WindowResizeEvent>(WLD_BIND_EVENT_FN(GameLayer::OnWindowResize));
	}
	bool GameLayer::OnWindowResize(WindowResizeEvent& e)
	{
		// 当独立游戏窗口缩放时，必须同步缩放 Scene 的摄像机 Aspect Ratio
		if (e.GetWidth() == 0 || e.GetHeight() == 0)
			return false; // 最小化时跳过

		m_ActiveScene->OnViewportResize(e.GetWidth(), e.GetHeight());
		return false;
	}

	void GameLayer::LoadScene()
	{
		m_ActiveScene = CreateRef<Scene>(*m_Context);
		SceneSerializer serializer(m_ActiveScene);
		// During the cook process, scenes could be packed or placed in content folder.
		// Assuming "Resource/Scenes/TestScene.wdscene" relative path is maintained or packed in pak.
		std::string scenePath = "scenes/PhysicalTest.wd";
		if (serializer.Deserialize(scenePath))
		{
			WLD_CORE_INFO("Scene loaded successfully from VFS or Disk!");

			uint32_t width = Application::Get().GetWindow().GetWidth();
			uint32_t height = Application::Get().GetWindow().GetHeight();
			m_ActiveScene->OnViewportResize(width, height);
			m_ActiveScene->OnRuntimeStart();
		}
		else
		{
			WLD_CORE_ERROR("Failed to load scene from VFS or Disk: {0}", scenePath);
		}
	}
}
