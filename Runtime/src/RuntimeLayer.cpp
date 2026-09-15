#include "RuntimeLayer.h"
#include "GameHud.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Renderer/Renderer.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/RHI/RhiTextureBridge.h"
#include "World/WUI/WuiRhiBackend.h"
#include "World/WUI/WuiTextureRegistry.h"

#include <cstdlib>

namespace World
{
	RuntimeLayer::RuntimeLayer()
		:Layer("RuntimeLayer")
	{

	}
	void RuntimeLayer::OnAttach()
	{
		WLD_PROFILE_FUNCTION();

		std::string moduleError;
		if (!Modules::GameModuleHost::LoadDefault(Application::Get().GetContext(), &moduleError))
			WLD_CORE_ERROR("Failed to load Game module: {0}", moduleError);

		// 会话描述:项目清单是唯一事实源(内容根/启动场景/后端),也是关卡清单(levels.welevel)的定位依据。
		Gameplay::GameAppDesc desc;
		desc.ProjectId = "worldengine-runtime";
		desc.FixedStepHz = 60;
		std::string scenePath = "scenes/test.wd";
		std::filesystem::path manifestPath;
		if (Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			std::string manifestError;
			Asset::ProjectManifest manifest;
			if (Asset::ProjectManifest::Load(manifestPath, &manifest, &manifestError))
			{
				if (!manifest.Id.empty())
					desc.ProjectId = manifest.Id;
				desc.ContentRoot = manifest.ResolveContentRoot(manifestPath);
				desc.StartLevel = manifest.StartScene;
				scenePath = manifest.StartScene;
				World::Renderer::SetRequestedRenderer(manifest.Renderer);
			}
		}

		m_SceneRenderer = CreateRef<SceneRenderer>();

		m_SceneRenderer->Init();
		m_SceneTextureId = Wui::WuiTextureRegistry::Get().Register(m_SceneRenderer->GetColorTexture());

		m_Host.Init(desc);
		m_Host.SetRenderer(m_SceneRenderer);

		// GameHost 内部:清单中存在同一场景的关卡时走 LevelService(加载状态机/进度),否则退回路径加载。
		m_Host.LoadLevel(scenePath, true);
	}
	void RuntimeLayer::OnDetach()
	{
		WLD_PROFILE_FUNCTION();
		// 顺序与改造前一致:先停运行时并释放场景,再关闭渲染器。
		m_Host.StopRuntime();
		m_Host.Shutdown();
		m_SceneTextureId = 0;
		if (m_SceneRenderer)
			m_SceneRenderer->Shutdown();
		m_SceneRenderer.reset();
	}
	void RuntimeLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();
		Renderer2D::ResetStats();
		// 场景更新(OnUpdateRuntime)+ 主相机提交渲染都在 GameHost 内完成,顺序与改造前一致。
		m_Host.Tick(ts, true);

		// 渲染基线捕获(仅开发验证):WLD_CAPTURE_FRAMES=N 后读默认帧缓冲写 PPM。
		CaptureFrameIfRequested();
	}

	void RuntimeLayer::CaptureFrameIfRequested()
	{
		static int countdown = -1;
		if (countdown == -1)
		{
			const char* frames = std::getenv("WLD_CAPTURE_FRAMES");
			countdown = frames ? std::atoi(frames) : -2;
		}
		if (countdown > 0)
		{
			--countdown;
			return;
		}
		if (countdown != 0)
			return;
		countdown = -2;

		const char* pathEnv = std::getenv("WLD_CAPTURE_PATH");
		if (!pathEnv || !pathEnv[0])
			return;
		if (m_SceneRenderer)
			m_SceneRenderer->CaptureFrame(pathEnv);
	}
	void RuntimeLayer::OnUiFrame()
	{
		static Wui::WuiContext wuiContext;
		static Wui::WuiRhiBackend wuiBackend;
		Wui::WuiInputState input;
		if (wuiBackend.BeginFrame(input))
		{
			wuiContext.BeginFrame(input);
			// 场景全屏显示:离屏颜色附件作为图像画进呈现目标,HUD 随后叠画。
			if (m_SceneTextureId)
				wuiContext.Commands().push_back({ Wui::WuiDrawKind::Image,
					{ 0, 0, input.ViewportSize.x, input.ViewportSize.y },
					{ 1, 1, 1, 1 }, 0, 1.0f, "", 15.0f, false,
					m_SceneTextureId, { 0, 1, 1, -1 }, -1, -1, -1 });
			// 只读查询必须走 const 路径:Running 场景上非 const GetRegistry()
			// 会触发结构写断言并抛异常。
			const Scene* activeScene = m_Host.GetScene().get();
			const size_t entityCount = activeScene ? activeScene->GetRegistry().view<UUIDComponent>().size() : 0;
			DrawGameHud(wuiContext, entityCount);
			wuiContext.EndFrame();
			wuiBackend.Render(wuiContext.Commands(), wuiContext.OverlayCommands());
		}
		wuiBackend.EndFrame(wuiContext.Cursor());
	}
	void RuntimeLayer::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		// 拦截窗口 Resize 事件以动态更新相机投影矩阵
		dispatcher.Dispatch<WindowResizeEvent>(WLD_BIND_EVENT_FN(RuntimeLayer::OnWindowResize));
	}
	bool RuntimeLayer::OnWindowResize(WindowResizeEvent& e)
	{
		// 当独立游戏窗口缩放时，必须同步缩放 Scene 的摄像机 Aspect Ratio
		if (e.GetWidth() == 0 || e.GetHeight() == 0)
			return false; // 最小化时跳过

		if (const Ref<Scene> scene = m_Host.GetScene())
			scene->OnViewportResize(e.GetWidth(), e.GetHeight());
		return false;
	}

}
