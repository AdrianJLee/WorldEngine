#include "GameLayer.h"
#include "World/Asset/ProjectManifest.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/Renderer/Legacy/Gfx/RenderCommand.h"

namespace World
{
	GameLayer::GameLayer(WorldContext& context)
		: Layer("GameLayer"), m_Context(&context)
	{

	}
	void GameLayer::OnAttach()
	{
		WLD_TRACE_FUNCTION();
		m_SceneRenderer = CreateRef<SceneRenderer>();

		m_SceneRenderer->Init();

		// Game.dll 的宿主层与会话:与 RuntimeLayer 走同一条 GameApp/GameHost 路径。
		// 启动关卡只认项目清单的 start_scene(Level 2.0 关卡的入口文件,如
		// `scenes/3DTest.wlevel`)—— 这里**不再**硬编码路径:原先写死的
		// `scenes/PhysicalTest.wd` 在任何模板里都不存在,宿主每次启动都静默加载失败。
		Gameplay::GameAppDesc desc;
		desc.ProjectId = "worldengine-game";
		desc.FixedStepHz = 60;
		desc.ContentRoot = std::filesystem::current_path();
		std::filesystem::path manifestPath;
		if (Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			Asset::ProjectManifest manifest;
			std::string manifestError;
			if (Asset::ProjectManifest::Load(manifestPath, &manifest, &manifestError))
			{
				if (!manifest.Id.empty())
					desc.ProjectId = manifest.Id;
				desc.ContentRoot = manifest.ResolveContentRoot(manifestPath);
				desc.StartLevel = manifest.StartScene;
			}
			else
			{
				WLD_CORE_ERROR("GameLayer: project manifest '{0}' rejected: {1}",
					manifestPath.string(), manifestError);
			}
		}
		m_Host.Init(desc);
		m_Host.SetRenderer(m_SceneRenderer);

		LoadLevel();

	}
	void GameLayer::OnDetach()
	{
		WLD_TRACE_FUNCTION();
		m_Host.StopRuntime();
		m_Host.Shutdown();
		m_SceneRenderer->Shutdown();
	}
	void GameLayer::OnUpdate(Timestep ts)
	{
		WLD_TRACE_FUNCTION();
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
		// 关卡来源 = 会话描述里的 StartLevel(清单的 start_scene,在 OnAttach 里解析)。
		// 清单缺失或没写 start_scene ⇒ 空路径,由 GameHost::LoadLevel 报可读错误,而不是
		// 偷偷去跑一个不存在的硬编码路径。
		const std::string scenePath = Gameplay::GameApp::Get().Desc().StartLevel;
		if (scenePath.empty())
		{
			WLD_CORE_ERROR("GameLayer: no start level — project.we.yaml is missing or has no start_scene; "
				"not loading any level");
			return;
		}
		// 加载失败时保持当前场景不变;无相机告警/视口同步/运行时启动由 GameHost 统一处理。
		m_Host.LoadLevel(scenePath, true);
	}
}
