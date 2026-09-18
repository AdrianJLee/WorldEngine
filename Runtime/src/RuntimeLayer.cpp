#include "RuntimeLayer.h"
#include "GameHud.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Log.h"
#include "World/Renderer/Renderer.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Renderer/SceneRenderer.h"
#include "World/RHI/RhiTextureBridge.h"
#include "World/Scene/ScriptEngine.h"
#include "World/WUI/WuiRhiBackend.h"
#include "World/WUI/WuiScriptedInput.h"
#include "World/WUI/WuiTextureRegistry.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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
				// D8a2:项目级渲染设置(rendering.*)对打包运行同样生效 —— 打包产物里没有
				// 环境变量,用户只能通过清单配置,所以这条路径必须显式 Apply。
				World::RenderSettings::Apply(manifest);
			}
			// 启动解析结果:定位到哪份清单/内容根/启动场景 —— 排查"跑了但没画面"的第一步。
			WLD_CORE_INFO("[runtime] manifest '{0}' contentRoot '{1}' startScene '{2}'",
				manifestPath.string(), desc.ContentRoot.string(), scenePath);
		}
		else
		{
			WLD_CORE_WARN("[runtime] no project.we.yaml found from cwd '{0}'; falling back to '{1}'",
				std::filesystem::current_path().string(), scenePath);
		}
		// 开发/验证钩子:WLD_START_SCENE 覆盖启动场景(与编辑器同名开关一致)。
		// 端到端验收(verify-play-vs-runtime.ps1)要靠它让 Runtime 跑指定的测试关卡;
		// 此前 Runtime 只认清单里的 start_scene,脚本传 2DTest 时它仍在跑 3DTest。
		if (const char* fromEnvironment = std::getenv("WLD_START_SCENE"))
			if (fromEnvironment[0])
			{
				scenePath = fromEnvironment;
				desc.StartLevel = scenePath;
				WLD_CORE_INFO("[runtime] WLD_START_SCENE override: {0}", scenePath);
			}

		m_SceneRenderer = CreateRef<SceneRenderer>();

		m_SceneRenderer->Init();
		// 场景目标跟随窗口尺寸:SceneRenderer 默认 1280×720,窗口被 WLD_WINDOW_SIZE 或用户
		// 缩放改变时必须跟上,否则画面被拉伸、与编辑器 Play 的像素对比也不成立。
		{
			const uint32_t width = Application::Get().GetWindow().GetWidth();
			const uint32_t height = Application::Get().GetWindow().GetHeight();
			if (width > 0 && height > 0)
				m_SceneRenderer->OnResize(width, height);
		}
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
		// 窗口尺寸变化:等尺寸稳定约 0.1s 再重建渲染目标(拖拽缩放时逐帧重建会在 Vulkan 下
		// 与在飞帧抢资源,和编辑器侧同一套节流策略)。
		if (m_SceneRenderer)
		{
			const uint32_t width = Application::Get().GetWindow().GetWidth();
			const uint32_t height = Application::Get().GetWindow().GetHeight();
			if (width > 0 && height > 0 && (m_SceneRenderer->GetWidth() != width || m_SceneRenderer->GetHeight() != height))
			{
				if (m_PendingWidth != width || m_PendingHeight != height)
				{
					m_PendingWidth = width;
					m_PendingHeight = height;
					m_ResizeDelay = 0.1f;
				}
				else
				{
					m_ResizeDelay -= ts.GetSeconds();
					if (m_ResizeDelay <= 0.0f)
					{
						m_SceneRenderer->OnResize(width, height);
						if (m_SceneTextureId)
							Wui::WuiTextureRegistry::Get().Update(m_SceneTextureId, m_SceneRenderer->GetColorTexture());
					}
				}
			}
		}
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
		// 帧数不是时间:先等关卡真正加载出场景,再开始倒计时(否则 GL 无 VSync 时
		// 会在场景就绪前就截到"只有清屏色"的画面)。
		if (countdown > 0 && !m_Host.GetScene())
			return;
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
		{
			// 诊断:确认抓取用的 SceneRenderer 与 GameHost 实际渲染的是同一个实例。
			WLD_CORE_INFO("[capture] runtime renderer={0} target={1}x{2}",
				static_cast<const void*>(m_SceneRenderer.get()),
				m_SceneRenderer->GetWidth(), m_SceneRenderer->GetHeight());
			m_SceneRenderer->CaptureFrame(pathEnv);
		}
	}
	void RuntimeLayer::OnUiFrame()
	{
		static Wui::WuiContext wuiContext;
		static Wui::WuiRhiBackend wuiBackend;
		++m_UiFrame;
		Wui::WuiInputState input;
		if (wuiBackend.BeginFrame(input))
		{
			// 开发钩子:脚本点击注入 → 本窗口输入状态。必须在 BeginFrame 之后、控件绘制之前,
			// 走的是和鼠标同一条输入路径(不是测试专用分支)。
			ApplyDevUiActions();
			Wui::WuiScriptedInput::Get().Apply("main", input);
			wuiContext.BeginFrame(input);
			// 场景全屏显示:离屏颜色附件作为图像画进呈现目标,HUD 随后叠画。
			if (m_SceneTextureId)
				wuiContext.Commands().push_back({ Wui::WuiDrawKind::Image,
					{ 0, 0, input.ViewportSize.x, input.ViewportSize.y },
					{ 1, 1, 1, 1 }, 0, 1.0f, "", 15.0f, false,
					m_SceneTextureId, { 0, 1, 1, -1 }, -1, -1, -1 });
			// 脚本 UI(W3c):在场景纹理之后、游戏 HUD 之前画;HUD 作为最上层覆盖,
			// 与编辑器侧"场景 → 面板 → 覆盖层"的层次一致。单个脚本出错只停自身。
			if (Scene* activeScene = m_Host.GetScene().get())
			{
				const std::size_t commandCountBefore = wuiContext.Commands().size();
				const std::size_t scriptUiFailures = ScriptEngine::DrawScriptUi(*activeScene, wuiContext);
				// 只有"脚本真的提交了控件且没有出错"才算画过(用于报告/自动化断言)。
				if (wuiContext.Commands().size() > commandCountBefore && scriptUiFailures == 0)
					m_ScriptUiDrawn = true;
			}
			// 只读查询必须走 const 路径:Running 场景上非 const GetRegistry()
			// 会触发结构写断言并抛异常。
			const Scene* activeScene = m_Host.GetScene().get();
			const size_t entityCount = activeScene ? activeScene->GetRegistry().view<UUIDComponent>().size() : 0;
			DrawGameHud(wuiContext, entityCount);
			wuiContext.EndFrame();
			wuiBackend.Render(wuiContext.Commands(), wuiContext.OverlayCommands());
			// 整窗抓图:UI 通道已提交、→Present 尚未执行 —— 与 EditorLayer 同一调用点
			// (见 Renderer::FlushPresentCaptures 的说明;提前抓会拍到空白并破坏布局)。
			Renderer::FlushPresentCaptures();
			FinishDevUiFrame();
		}
		wuiBackend.EndFrame(wuiContext.Cursor());
	}

	namespace
	{
		// W3c 宿主接线:纯开发开关(命名沿用 Editor 侧习惯),发布路径默认全部关闭。
		//   WLD_UI_CLICK="x,y"            注入一次脚本点击(press + release 两帧,与编辑器一致)
		//   WLD_UI_CLICK_FRAME=<n>        点击发生在第 n 个 UI 帧(1 基;第 1 个 UI 帧 = 1)
		//   WLD_UI_CLICK_QUIT=<n>         点击后第 n 个 UI 帧自动退出
		//   WLD_CAPTURE_PRESENT=<path>    整窗抓图写 PPM(绝对路径,或相对当前工作目录)
		//   WLD_CAPTURE_PRESENT_FRAME=<n> 抓图发生在第 n 个 UI 帧
		//   WLD_CAPTURE_PRESENT_QUIT=<n>  抓图落盘后第 n 个 UI 帧自动退出(0 = 落盘即退)
		struct DevUiConfig
		{
			glm::vec2 ClickPosition { 0, 0 };
			bool HasClick = false;
			int ClickFrame = 30;
			int ClickQuit = 0;
			std::string CapturePath;
			int CaptureFrame = 40;
			int CaptureQuit = 0;
		};

		struct DevUiState
		{
			DevUiConfig Config;
			bool ClickInjected = false;
			bool CaptureRequested = false;
		};

		// 进程内单例:Runtime 只有一个 RuntimeLayer,环境变量在进程生命周期内不变。
		DevUiState& DevUi()
		{
			static DevUiState state = []()
			{
				DevUiState fresh;
				DevUiConfig& config = fresh.Config;
				if (const char* click = std::getenv("WLD_UI_CLICK"))
				{
					float x = 0.0f;
					float y = 0.0f;
					if (std::sscanf(click, "%f,%f", &x, &y) == 2)
					{
						config.ClickPosition = { x, y };
						config.HasClick = true;
					}
					else
					{
						WLD_CORE_WARN("[dev-ui] WLD_UI_CLICK='{0}' is not \"x,y\"; click injection disabled", click);
					}
				}
				if (const char* frame = std::getenv("WLD_UI_CLICK_FRAME"))
					config.ClickFrame = std::atoi(frame);
				if (const char* quit = std::getenv("WLD_UI_CLICK_QUIT"))
					config.ClickQuit = std::atoi(quit);
				if (const char* capture = std::getenv("WLD_CAPTURE_PRESENT"))
					config.CapturePath = capture;
				if (const char* frame = std::getenv("WLD_CAPTURE_PRESENT_FRAME"))
					config.CaptureFrame = std::atoi(frame);
				if (const char* quit = std::getenv("WLD_CAPTURE_PRESENT_QUIT"))
					config.CaptureQuit = std::atoi(quit);

				if (config.HasClick)
					WLD_CORE_INFO("[dev-ui] click ({0},{1}) queued at UI frame {2} (quit +{3})",
						config.ClickPosition.x, config.ClickPosition.y, config.ClickFrame, config.ClickQuit);
				if (!config.CapturePath.empty())
					WLD_CORE_INFO("[dev-ui] present capture '{0}' requested at UI frame {1} (quit +{2})",
						config.CapturePath, config.CaptureFrame, config.CaptureQuit);
				return fresh;
			}();
			return state;
		}
	}

	void RuntimeLayer::ApplyDevUiActions()
	{
		DevUiState& devUi = DevUi();

		if (devUi.Config.HasClick && !devUi.ClickInjected &&
			static_cast<int>(m_UiFrame) >= devUi.Config.ClickFrame)
		{
			Wui::WuiScriptedInput::Get().QueueClick("main", devUi.Config.ClickPosition);
			devUi.ClickInjected = true;
			// 自动退出安排在点击被消费之后足够帧数(默认 0 → 下一个 UI 帧),让脚本状态先落盘。
			m_DevUiArmedFrame = m_UiFrame + 1 + static_cast<uint32_t>(std::max(devUi.Config.ClickQuit, 0));
			WLD_CORE_INFO("[dev-ui] injected scripted UI click at ({0},{1}) on UI frame {2}",
				devUi.Config.ClickPosition.x, devUi.Config.ClickPosition.y, m_UiFrame);
		}

		// 抓图请求:与编辑器同样的"登记 + 帧末 flush"两步,不在这里读像素。
		if (!devUi.Config.CapturePath.empty() && !devUi.CaptureRequested &&
			static_cast<int>(m_UiFrame) >= devUi.Config.CaptureFrame)
		{
			devUi.CaptureRequested = true;
			m_DevUiCapturePending = true;
			m_DevUiArmedFrame = m_UiFrame + 1 + static_cast<uint32_t>(std::max(devUi.Config.CaptureQuit, 0));
			Renderer::RequestPresentCapture(nullptr, devUi.Config.CapturePath,
				Application::Get().GetWindow().GetWidth(), Application::Get().GetWindow().GetHeight());
			WLD_CORE_INFO("[dev-ui] present capture queued on UI frame {0}: {1}", m_UiFrame, devUi.Config.CapturePath);
		}
	}

	void RuntimeLayer::FinishDevUiFrame()
	{
		if (!m_DevUiCapturePending || m_UiFrame < m_DevUiArmedFrame)
			return;

		const std::string& capturePath = DevUi().Config.CapturePath;
		std::error_code fileError;
		const uintmax_t size = std::filesystem::file_size(capturePath, fileError);
		if (fileError || size == 0)
		{
			// 抓图没落盘(例如 Vulkan 呈现目标尚未就绪)时不要静默退出:留下证据、
			// 保持挂起等下一帧重试,由验证脚本判定"没有 PPM = 失败"。
			WLD_CORE_ERROR("[dev-ui] present capture '{0}' was not written (ui frame {1})", capturePath, m_UiFrame);
			m_DevUiArmedFrame = m_UiFrame + 1;
			return;
		}
		WLD_CORE_INFO("[dev-ui] present capture written (ui frame {0}, {1} bytes): {2}",
			m_UiFrame, size, capturePath);
		m_DevUiCapturePending = false;
		Application::Get().Close();
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
