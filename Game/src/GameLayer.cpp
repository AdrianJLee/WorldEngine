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

		// Game.dll 的宿主层与会话:与 RuntimeLayer 走同一条 GameApp/GameHost 路径。
		Gameplay::GameAppDesc desc;
		desc.ProjectId = "worldengine-game";
		desc.FixedStepHz = 60;
		desc.ContentRoot = std::filesystem::current_path();
		m_Host.Init(desc);
		m_Host.SetRenderer(m_SceneRenderer);

		LoadLevel();

	}
	void GameLayer::OnDetach()
	{
		WLD_PROFILE_FUNCTION();
		m_Host.StopRuntime();
		m_Host.Shutdown();
		m_SceneRenderer->Shutdown();
	}
	void GameLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();
		Renderer2D::ResetStats();
		// 与 RuntimeLayer 完全同路径:场景 OnUpdateRuntime → 主相机提交渲染。
		m_Host.Tick(ts, true);
	}
	void GameLayer::OnUiFrame()
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

		if (const Ref<Scene> scene = m_Host.GetScene())
			scene->OnViewportResize(e.GetWidth(), e.GetHeight());
		return false;
	}

	void GameLayer::LoadLevel()
	{
		// During the cook process, scenes could be packed or placed in content folder.
		// Assuming "Resource/Scenes/TestScene.wdscene" relative path is maintained or packed in pak.
		std::string scenePath = "scenes/PhysicalTest.wd";
		// 加载失败时保持当前场景不变;无相机告警/视口同步/运行时启动由 GameHost 统一处理。
		m_Host.LoadLevel(scenePath, true);
	}
}
