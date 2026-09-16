#include "EditorLayer.h"
#include "EditorCooker.h"
#include "EditorStartup.h"
#include "World/Core/Asset/BuiltinImporters.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Core/Vfs/DirectoryProvider.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Scene/ScriptEngine.h"
#include "World/WUI/WuiRhiBackend.h"
#include "World/WUI/WuiTextureRegistry.h"
#include <filesystem>
#include <shellapi.h>
#include <stdexcept>
#include <chrono>
#include "World/Events/MouseEvent.h"
namespace World
{
	EditorLayer::EditorLayer()
		: Layer("EditorLayer"), m_Document(Application::Get().GetContext()), m_Shell(*this)
	{
		m_Commands.Register({ Wui::HashId("cmd.new"), "New", KeyCodes::N, true, false, [this] { NewScene(); } });
		m_Commands.Register({ Wui::HashId("cmd.open"), "Open", KeyCodes::O, true, false, [this] { OpenScene(); } });
		m_Commands.Register({ Wui::HashId("cmd.save"), "Save", KeyCodes::S, true, false, [this] { SaveScene(); } });
		m_Commands.Register({ Wui::HashId("cmd.duplicate"), "Duplicate", KeyCodes::D, true, false, [this] { DuplicateSelectedEntity(); } });
		m_Commands.Register({ Wui::HashId("cmd.gizmo_none"), "Gizmo None", KeyCodes::Q, false, false, [this] { SetGizmoOperation(Wui::GizmoOperation::None); } });
		m_Commands.Register({ Wui::HashId("cmd.gizmo_move"), "Gizmo Move", KeyCodes::W, false, false, [this] { SetGizmoOperation(Wui::GizmoOperation::Translate); } });
		m_Commands.Register({ Wui::HashId("cmd.gizmo_rotate"), "Gizmo Rotate", KeyCodes::E, false, false, [this] { SetGizmoOperation(Wui::GizmoOperation::Rotate); } });
		m_Commands.Register({ Wui::HashId("cmd.gizmo_scale"), "Gizmo Scale", KeyCodes::R, false, false, [this] { SetGizmoOperation(Wui::GizmoOperation::Scale); } });
		m_Commands.Register({ Wui::HashId("cmd.play"), "Play", KeyCodes::F5, false, false, [this] { TogglePlay(); } });
		m_Commands.Register({ Wui::HashId("cmd.simulate"), "Simulate", KeyCodes::F6, false, false, [this] { ToggleSimulate(); } });
		m_Commands.Register({ Wui::HashId("cmd.pause"), "Pause", KeyCodes::F7, false, false, [this] { TogglePause(); } });
	}
	void EditorLayer::OnAttach()
	{
		WLD_PROFILE_FUNCTION();
		// 主窗口无边框:顶部第一行(挂靠栏)即窗口栏位,可拖动/关闭;边缘缩放保留。
		Application::Get().GetWindow().SetFrameless(true);
		std::string moduleError;
		if (!Modules::GameModuleHost::LoadDefault(Application::Get().GetContext(), &moduleError))
			WLD_CORE_ERROR("Failed to load Game module: {0}", moduleError);

		// 开发期资产:编辑器与 Runtime 一致,经 VFS 目录 provider 读内容。
		// 目录 provider 已由 Application::MountProjectContent 统一挂载,此处不再重复。

		// Application initialized Lua before attach; Game registration is now merged.
		if (!ScriptEngine::GenerateLuaStubs())
			WLD_CORE_ERROR("Automatic Lua API stub generation failed; keeping the last valid declarations.");

		m_SceneRenderer = CreateRef<SceneRenderer>();
		m_SceneRenderer->Init();

		// W8-3:编辑器侧存档服务(与 Runtime/Play 共用同一实现):项目 id 取项目清单,
		// 场景来源是当前活动场景 —— 内容作者可以在编辑态直接保存/读取状态做验证。
		{
			std::string projectId = "worldengine-editor";
			std::filesystem::path manifestPath;
			if (World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
			{
				World::Asset::ProjectManifest manifest;
				std::string manifestError;
				if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &manifestError) && !manifest.Id.empty())
					projectId = manifest.Id;
			}
			m_SaveService = std::make_unique<Gameplay::SaveService>(projectId,
				[this] { return m_ActiveScene.get(); });
		}

		LoadIconTextures();
		RegisterUiTextures();

		// 启动场景来源:-scene 参数(进程内传递,优先)或 WLD_START_SCENE 环境变量。
		std::string startScene = World::Editor::StartupScenePath();
		if (startScene.empty())
			if (const char* fromEnvironment = std::getenv("WLD_START_SCENE"))
				startScene = fromEnvironment;
		if (!startScene.empty())
		{
			const std::filesystem::path startupPath(startScene);
			WLD_CORE_INFO("Startup scene requested: {0}", startupPath.string());
			DoOpenScene(startupPath);
			if (!m_Document.HasPath())
				WLD_CORE_ERROR("Startup scene could not be opened: {0} ({1})",
					startupPath.string(), m_Document.GetLastError());
		}
		else
			NewScene();

		m_EditorCamera = EditorCamera(45.0f, 1.6f / 0.9f, 0.1f, 1000.0f);
		// D7-1a:3D 视口相机(轨道/飞行)。默认关闭;WLD_VIEWPORT_3D=1 便于自动化验证,
		// 工具栏切换在 ViewportPanel 接入(下一步)。
		m_EditorCamera3D = EditorCamera3D(60.0f, 1.6f / 0.9f, 0.1f, 2000.0f, 12.0f);
		m_EditorCamera3D.SetViewportSize(1280, 720);
		m_Viewport3D = std::getenv("WLD_VIEWPORT_3D") != nullptr;
	}

	// 图标是旧式(GL)纹理:窗口/上下文重建后必须重新加载,否则渲染出的图标会错乱。
	void EditorLayer::LoadIconTextures()
	{
		m_IconPlay = Texture2D::Create("Resource/Icons/Icon_Play.png");

		m_IconStop = Texture2D::Create("Resource/Icons/Icon_Stop.png");

		m_IconPause = Texture2D::Create("Resource/Icons/Icon_Pause.png");
		m_IconContinue = Texture2D::Create("Resource/Icons/Icon_Continue.png");

		m_IconSimulate = Texture2D::Create("Resource/Icons/Icon_SimulateStart.png");
		m_IconSimulateStop = Texture2D::Create("Resource/Icons/Icon_SimulateStop.png");
		m_IconSimulatePause = Texture2D::Create("Resource/Icons/Icon_SimulatePause.png");
		m_IconSimulateContinue = Texture2D::Create("Resource/Icons/Icon_SimulateContinue.png");
	}

	void EditorLayer::OnDetach()
	{
		WLD_PROFILE_FUNCTION();
		if (m_CookingThread.joinable())
			m_CookingThread.join();
		m_ShowCookingProgress = false;
		m_HasRenderedScene = false;

		SetSceneState(SceneState::Edit);
		
		m_ActiveScene.reset();
		m_RuntimeScene.reset();
		m_Document = EditorDocument(Application::Get().GetContext());
		if (m_SceneRenderer)
		{
			m_SceneRenderer->Shutdown();
			m_SceneRenderer.reset();
		}
		// 独立窗口(含附加状态)必须先于 RHI 设备/主窗口销毁,否则关闭引擎时会崩。
		m_Shell.ReleaseIndependentWindows();
		ExportOperationLog();
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();
		ProcessPendingRendererChange();
		m_HasRenderedScene = false;
		// 视口目标重建节流:尺寸稳定(约 100ms)后再真正重建渲染目标。
		if (m_PendingViewportSize.x > 0 && m_PendingViewportSize.y > 0)
		{
			m_ViewportResizeDelay -= ts.GetSeconds();
			if (m_ViewportResizeDelay <= 0.0f)
			{
				const glm::vec2 size = m_PendingViewportSize;
				m_PendingViewportSize = { 0, 0 };
				if (m_ViewportSize != size)
				{
					m_ViewportSize = size;
					if (m_SceneRenderer)
						m_SceneRenderer->GetTargetFramebuffer()->Resize((uint32_t)size.x, (uint32_t)size.y);
					if (m_ActiveScene)
						m_ActiveScene->OnViewportResize((uint32_t)size.x, (uint32_t)size.y);
				}
			}
		}
		if (!m_ActiveScene || !m_SceneRenderer)
			return;
		// 开发/验证钩子:WLD_AUTOPLAY=<帧数> 时在该帧自动进入 Play(等价于点视图口播放按钮),
		// 供隐藏冒烟与回归脚本验证 Play 路径(与 WLD_START_SCENE/WLD_CAPTURE_FRAMES 同类)。
		if (const char* autoPlayFrames = std::getenv("WLD_AUTOPLAY"))
		{
			static int devFrame = 0;
			static bool devAutoPlayDone = false;
			const int target = std::atoi(autoPlayFrames);
			if (!devAutoPlayDone && target > 0 && ++devFrame >= target)
			{
				devAutoPlayDone = true;
				TogglePlay();
				WLD_CORE_INFO("[dev] WLD_AUTOPLAY: entered Play after {0} frames", devFrame);
			}
		}
		RunHierarchyClickCheck();
		//WLD_CORE_TRACE("Delta Time: {0} ({1} FPS)", ts.GetSeconds(), ts.GetFPS());

		{
			// TODO: 停止聚焦时，摄像机不再更新,这不太合理，应该让摄像机在停止聚焦时继续更新，但不处理输入事件
			if (m_ViewportFocused)
			{
				m_EditorCamera.OnUpdate(ts);
			}
		}

		Renderer2D::ResetStats();


		WLD_PROFILE_SCOPE("Renderer Clear");
		Camera* renderCamera = &m_EditorCamera;
		glm::mat4 renderCameraTransform = m_EditorCamera.GetTransform();
		// D7-1a:3D 模式用 EditorCamera3D 的投影/视图(P1b D1 的相机),沿用同一个提交接口。
		Camera viewportCamera3D { m_EditorCamera3D.GetProjectionMatrix(false) };
		if (m_Viewport3D)
		{
			renderCamera = &viewportCamera3D;
			renderCameraTransform = glm::inverse(m_EditorCamera3D.GetViewMatrix());
		}


		{
			WLD_PROFILE_SCOPE("Renderer Draw");
			switch (m_SceneState)
			{
				case SceneState::Edit:
					m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
					break;
				case SceneState::Play:
					if (!m_ScenePaused)
					{
						// 与 Runtime 同一条路径:GameApp 驱动阶段回调(GameHost 内注册的 update
						// 会调用场景 OnUpdateRuntime);渲染仍由编辑器视图口统一提交(render=false)。
						m_PlayHost.Tick(ts, /*render=*/false);
					}
					else
						m_ActiveScene->FlushStructuralChanges();
					break;
				case SceneState::Simulate:
					if (!m_ScenePaused)
						m_ActiveScene->OnUpdateSimulation(ts, m_EditorCamera);
					else
						m_ActiveScene->FlushStructuralChanges();
					break;
			}

		}

		Entity selectedEntity = m_SelectedEntity;
		if (!selectedEntity.IsValid() || selectedEntity.GetScene() != m_ActiveScene.get() ||
			m_ActiveScene->IsPendingDestroy(selectedEntity))
		{
			// 诊断(WLD_TRACE_UI=1):说明"点击层级后属性面板显示 No entity selected"是
			// 哪一条校验把选择清掉了(Play 下活动场景是播放副本,指针会随播放切换)。
			// 只在"确实有过一个选择"时打:空选择是正常态,不然每帧都会刷。
			if (selectedEntity.IsValid() && std::getenv("WLD_TRACE_UI"))
			{
				static int traced = 0;
				if (traced < 12)
				{
					++traced;
					// 注意:spdlog 的实参会无条件求值,Entity::GetScene() 对无效句柄会抛异常
					// (RequireValid),必须先把指针算好,否则这条诊断自己会把进程干掉。
					WLD_CORE_INFO("[ui] selection cleared: valid={0} entityScene={1} activeScene={2} pendingDestroy={3}",
						1,
						static_cast<const void*>(selectedEntity.GetScene()),
						static_cast<const void*>(m_ActiveScene.get()),
						m_ActiveScene->IsPendingDestroy(selectedEntity) ? 1 : 0);
				}
			}
			m_SelectedEntity = {};
			selectedEntity = {};
		}
		else if (!selectedEntity.HasComponent<TransformComponent>())
			selectedEntity = {}; // Keep Inspector selection, but omit the renderer's outline.

		// No component reference survives Scene update or its structural flush.
		if (m_SceneState == SceneState::Play && !m_ScenePaused)
		{
			Entity cameraEntity = m_ActiveScene->GetPrimaryCameraEntity();
			if (!cameraEntity.IsValid() || m_ActiveScene->IsPendingDestroy(cameraEntity) ||
				!cameraEntity.HasComponent<CameraComponent>() || !cameraEntity.HasComponent<TransformComponent>())
				return;
			renderCamera = &cameraEntity.GetComponent<CameraComponent>().Camera;
			renderCameraTransform = cameraEntity.GetComponent<TransformComponent>().Transform;
		}

		m_SceneRenderer->BeginScene(m_ActiveScene.get(), m_RendererOptions);
		m_SceneRenderer->SubmitScene(*renderCamera, renderCameraTransform, selectedEntity);
		m_SceneRenderer->EndScene();
		m_HasRenderedScene = true;
		// 开发验证:像素基线截图(与 Runtime 同名开关)。走的是后端无关的 RHI 读回,
		// Vulkan/GL 都能抓到本帧场景颜色附件。
		CaptureFrameIfRequested();
		// 点选校验必须在场景渲染之后:m_HasRenderedScene 在 OnUpdate 开头被复位,
		// 放在前面会让 GetEntityAtMousePosition 直接早退(等于没测)。
		RunPickCheck();
	}

	void EditorLayer::CaptureFrameIfRequested()
	{
		static const auto startTime = std::chrono::steady_clock::now();
		static int countdown = -1;
		if (countdown == -1)
		{
			const char* frames = std::getenv("WLD_CAPTURE_FRAMES");
			countdown = frames ? std::atoi(frames) : -2;
		}
		// 帧数不是时间:GL 无 VSync 时帧率远高于 Vulkan(实测同一帧数下 GL 还没建立视口、
		// Vulkan 已经跑了 6 秒)。因此先等一段墙钟时间(默认 2s,可用
		// WLD_CAPTURE_DELAY_SECONDS 覆盖)再开始按帧倒计时,两个后端才可比。
		if (countdown > 0)
		{
			const char* delayEnv = std::getenv("WLD_CAPTURE_DELAY_SECONDS");
			const double delay = delayEnv ? std::atof(delayEnv) : 2.0;
			const double elapsed = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - startTime).count();
			if (elapsed < delay)
				return;
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
		if (!pathEnv || !pathEnv[0] || !m_SceneRenderer)
			return;
		WLD_CORE_INFO("[capture] scene target {0}x{1}, viewport {2}x{3}",
			m_SceneRenderer->GetWidth(), m_SceneRenderer->GetHeight(),
			static_cast<uint32_t>(m_ViewportSize.x), static_cast<uint32_t>(m_ViewportSize.y));
		m_SceneRenderer->CaptureFrame(pathEnv);
	}

	void EditorLayer::RunHierarchyClickCheck()
	{
		// 自动化复现:WLD_HIERARCHY_CLICK=<进入 Play 后的帧数>(缺省 30,最小 3)。
		// 调用的是层级面板行控件真实的 OnClick 回调,再等 3 帧确认选择没有被
		// "实体必须属于活动场景" 的校验清掉(Play 的活动场景是 CopyScene 的播放副本)。
		if (m_DevClickFramesAfterPlay == -1)
		{
			const char* requested = std::getenv("WLD_HIERARCHY_CLICK");
			if (!requested || !requested[0])
			{
				m_DevClickFramesAfterPlay = -2; // 未启用
				return;
			}
			const int frames = std::atoi(requested);
			m_DevClickFramesAfterPlay = frames < 3 ? 3 : frames;
		}
		if (m_DevClickFramesAfterPlay == -2 || m_SceneState != SceneState::Play)
			return;

		if (m_DevClickVerifyCountdown >= 0)
		{
			if (--m_DevClickVerifyCountdown > 0)
				return;
			const bool selected = m_SelectedEntity.IsValid() && m_SelectedEntity.GetScene() == m_ActiveScene.get();
			if (selected)
				WLD_CORE_INFO("[dev] hierarchy-click check: PASS (handle={0}, scene={1})",
					static_cast<uint32_t>(static_cast<entt::entity>(m_SelectedEntity)),
					static_cast<const void*>(m_SelectedEntity.GetScene()));
			else
				WLD_CORE_ERROR("[dev] hierarchy-click check: FAIL (valid={0}, selectedScene={1}, activeScene={2})",
					m_SelectedEntity.IsValid() ? 1 : 0,
					m_SelectedEntity.IsValid() ? static_cast<const void*>(m_SelectedEntity.GetScene()) : nullptr,
					static_cast<const void*>(m_ActiveScene.get()));
			Application::Get().Close();
			return;
		}

		if (++m_DevClickPlayFrames < m_DevClickFramesAfterPlay)
			return;
		if (!m_Shell.DebugClickHierarchyRow(0))
		{
			WLD_CORE_ERROR("[dev] hierarchy-click check: FAIL (no hierarchy row available; panel hidden?)");
			Application::Get().Close();
			return;
		}
		WLD_CORE_INFO("[dev] hierarchy-click: invoked row 0 click after {0} Play frames", m_DevClickPlayFrames);
		m_DevClickVerifyCountdown = 3;
	}

	void EditorLayer::RunPickCheck()
	{
		// D7-1c 自动化:WLD_PICK_AT="x,y;x,y;…"(视口局部坐标,左上角原点)。
		// 渲染稳定后逐点拾取并打印 handle,随即退出 —— 双后端跑同一条命令,
		// 输出必须逐点一致(否则就是行序/读回路径不对)。
		if (m_DevPickFrames == -1)
		{
			const char* spec = std::getenv("WLD_PICK_AT");
			if (!spec || !spec[0])
			{
				m_DevPickFrames = -2; // 未启用
				return;
			}
			const char* framesEnv = std::getenv("WLD_PICK_FRAMES");
			m_DevPickFrames = framesEnv ? std::atoi(framesEnv) : 30;
			if (m_DevPickFrames < 3)
				m_DevPickFrames = 3;
			std::string text(spec);
			size_t start = 0;
			while (start <= text.size())
			{
				const size_t end = text.find(';', start);
				const std::string point = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
				float x = 0.0f, y = 0.0f;
				if (std::sscanf(point.c_str(), "%f,%f", &x, &y) == 2)
					m_DevPickPoints.push_back({ x, y });
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			if (m_DevPickPoints.empty())
			{
				WLD_CORE_ERROR("[dev] WLD_PICK_AT has no valid 'x,y' point: {0}", text);
				m_DevPickFrames = -2;
				return;
			}
		}
		if (m_DevPickFrames == -2 || m_SceneState != SceneState::Edit)
			return;
		if (++m_DevPickFrameCount < m_DevPickFrames)
			return;
		// 帧数不够可靠:GL 无 VSync 时 40 帧可能只花 0.04s,视口目标的重建节流(0.1s)还没生效,
		// 读回的 texel 与坐标会对不上。这里再等一段墙钟时间(默认 1s)。
		{
			static const auto s_Start = std::chrono::steady_clock::now();
			const char* delayEnv = std::getenv("WLD_PICK_DELAY_SECONDS");
			const double delay = delayEnv ? std::atof(delayEnv) : 1.0;
			const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_Start).count();
			if (elapsed < delay)
				return;
		}

		for (const glm::vec2& point : m_DevPickPoints)
		{
			static bool s_EnvLogged = false;
			if (!s_EnvLogged)
			{
				s_EnvLogged = true;
				WLD_CORE_INFO("[dev] pick env: backend={0} window={1}x{2} viewport={3}x{4} target={5}x{6} bounds=({7},{8})-({9},{10})",
					Renderer::GetBackendName(),
					Application::Get().GetWindow().GetWidth(), Application::Get().GetWindow().GetHeight(),
					static_cast<int>(m_ViewportSize.x), static_cast<int>(m_ViewportSize.y),
					m_SceneRenderer->GetWidth(), m_SceneRenderer->GetHeight(),
					static_cast<int>(m_ViewportBounds[0].x), static_cast<int>(m_ViewportBounds[0].y),
					static_cast<int>(m_ViewportBounds[1].x), static_cast<int>(m_ViewportBounds[1].y));
			}
			const Entity picked = GetEntityAtMousePosition(point);
			WLD_CORE_INFO("[dev] pick at ({0},{1}) -> handle={2} valid={3}",
				static_cast<int>(point.x), static_cast<int>(point.y),
				picked.IsValid() ? static_cast<uint32_t>(static_cast<entt::entity>(picked)) : 0u,
				picked.IsValid() ? 1 : 0);
		}
		Application::Get().Close();
	}


	void EditorLayer::OnUiFrame()
	{
		WLD_PROFILE_FUNCTION();

		static Wui::WuiRhiBackend wuiBackend;

		// 多窗口:每帧开始前显式把主窗口的 GL 上下文设为当前,
		// 避免上一帧独立窗口渲染留下的上下文影响主窗口的绘制与交换。
		Application::Get().GetWindow().MakeCurrent();

		Wui::WuiInputState input;
		if (wuiBackend.BeginFrame(input))
		{
			m_WuiContext.BeginFrame(input);
			// 注册表在设备切换/重建后会被清空(Generation 递增),此时需要
			// 重新注册图标与场景纹理,否则图像命令解析不到贴图(图标消失)。
			if (Wui::WuiTextureRegistry::Get().Generation() != m_UiTextureGeneration)
				RegisterUiTextures();
			if (m_SceneRenderer && m_SceneRenderer->GetColorTexture())
			{
				if (!m_SceneTextureId)
					m_SceneTextureId = Wui::WuiTextureRegistry::Get().Register(m_SceneRenderer->GetColorTexture());
				else
					Wui::WuiTextureRegistry::Get().Update(m_SceneTextureId, m_SceneRenderer->GetColorTexture());
			}
			m_Shell.OnRender(m_WuiContext);
			m_WuiContext.EndFrame();
			wuiBackend.Render(m_WuiContext.Commands(), m_WuiContext.OverlayCommands());
		}
		wuiBackend.EndFrame(m_WuiContext.Cursor());
	}

	void EditorLayer::ExportOperationLog()
	{
		const std::string path = std::string(WLD_OUTPUT_DIR) + "wui-ops.json";
		std::string error;
		if (m_WuiContext.Ops().Save(path, &error))
			WLD_CORE_INFO("WUI operation log exported to {0}", path);
		else
			WLD_CORE_WARN("Failed to export WUI operation log: {0}", error);
	}

	void EditorLayer::OnEvent(Event& event)
	{
		WLD_PROFILE_FUNCTION();

		if (m_ViewportFocused && m_ViewportHovered)
		{
			m_EditorCamera.OnEvent(event);
			// D7-1a:3D 视口的轨道/平移/推拉(右键 orbit、中键 pan、滚轮 dolly)。
			if (m_Viewport3D)
			{
				EventDispatcher cameraDispatcher(event);
				cameraDispatcher.Dispatch<MouseButtonPressedEvent>([this](MouseButtonPressedEvent& e)
				{
					m_Viewport3DDragging = e.GetMouseButton() == 1 /*右键*/ ? Viewport3DDrag::Orbit
						: (e.GetMouseButton() == 2 /*中键*/ ? Viewport3DDrag::Pan : Viewport3DDrag::None);
					return false;
				});
				cameraDispatcher.Dispatch<MouseButtonReleasedEvent>([this](MouseButtonReleasedEvent& e)
				{
					(void)e;
					m_Viewport3DDragging = Viewport3DDrag::None;
					return false;
				});
				cameraDispatcher.Dispatch<MouseMovedEvent>([this](MouseMovedEvent& e)
				{
					if (m_Viewport3DDragging == Viewport3DDrag::Orbit)
						m_EditorCamera3D.Orbit(e.GetX() - m_Viewport3DLastMouse.x, e.GetY() - m_Viewport3DLastMouse.y);
					else if (m_Viewport3DDragging == Viewport3DDrag::Pan)
						m_EditorCamera3D.Pan(e.GetX() - m_Viewport3DLastMouse.x, e.GetY() - m_Viewport3DLastMouse.y);
					m_Viewport3DLastMouse = { e.GetX(), e.GetY() };
					return false;
				});
				cameraDispatcher.Dispatch<MouseScrolledEvent>([this](MouseScrolledEvent& e)
				{
					m_EditorCamera3D.Dolly(-e.GetYOffset() * 0.6f);
					return false;
				});
			}
		}

		EventDispatcher dispatcher(event);

		dispatcher.Dispatch<WindowCloseEvent>(WLD_BIND_EVENT_FN(EditorLayer::OnWindowClose));
		dispatcher.Dispatch<KeyPressedEvent>(WLD_BIND_EVENT_FN(EditorLayer::OnKeyPressed));
	}
	bool EditorLayer::OnWindowClose(WindowCloseEvent& e)
	{
		// 确认框已打开：继续拦截关闭，等待用户在框内选择。
		if (m_ShowUnsavedModal)
		{
			e.m_Handled = true;
			return true;
		}
		if (m_Document.IsDirty())
		{
			RequestAction([this]() { World::Application::Get().Close(); });
			e.m_Handled = true;
			return true;
		}
		return false;
	}
	void EditorLayer::NewScene()
	{
		RequestAction([this]() { DoNewScene(); });
	}
	void EditorLayer::DoNewScene()
	{
		SetSceneState(SceneState::Edit);

		m_Document.New();
		UpdateSceneContext(m_Document.GetScene());
	}
	void EditorLayer::OpenScene()
	{
		std::string path = FileDialogs::OpenFile("Scene File (*.wd)\0*.wd\0");
		if (path.empty())
			return;
		OpenScene(std::filesystem::path(path));
	}
	void EditorLayer::OpenScene(const std::filesystem::path& path)
	{
		if (path.empty())
			return;
		RequestAction([this, path]() { DoOpenScene(path); });
	}
	void EditorLayer::DoOpenScene(const std::filesystem::path& path)
	{
		WLD_CORE_INFO("Opening scene: {0}", path.string());
		if (!m_Document.LoadFromFile(path))
		{
			ShowError(m_Document.GetLastError());
			return;
		}
		SetSceneState(SceneState::Edit);
		UpdateSceneContext(m_Document.GetScene());
	}
	bool EditorLayer::SaveScene()
	{
		if (TrySave())
			return true;
		if (!m_Document.GetLastError().empty())
			ShowError(m_Document.GetLastError());
		return false;
	}
	void EditorLayer::StartCookingAction()
	{
		const std::string target =
			World::FileDialogs::SelectFolder("Select the output folder for the game package");
		if (!target.empty())
			StartCooking(target);
	}

	void EditorLayer::GenerateLuaStubsAction()
	{
		if (!ScriptEngine::GenerateLuaStubs())
			WLD_CORE_ERROR("Lua API stub generation failed; keeping the last valid declarations.");
	}

	void EditorLayer::CloseAction()
	{
		RequestAction([this]() { World::Application::Get().Close(); });
	}

	void EditorLayer::DuplicateSelectedEntity()
	{
		Entity selectedEntity = m_SelectedEntity;
		if (!m_ActiveScene || !selectedEntity.IsValid() || selectedEntity.GetScene() != m_ActiveScene.get() ||
			m_ActiveScene->IsPendingDestroy(selectedEntity))
		{
			m_SelectedEntity = {};
			return;
		}
		try
		{
			if (m_ActiveScene->IsActive() &&
				(selectedEntity.HasComponent<RigidBody2DComponent>() || selectedEntity.HasComponent<BoxCollider2DComponent>() ||
					selectedEntity.HasComponent<CircleCollider2DComponent>()))
			{
				WLD_CORE_WARN("Cannot duplicate physics entities while the scene is active. Stop the scene first.");
				return;
			}
			const entt::entity handle = selectedEntity;
			if (m_ActiveScene->DeferStructuralChange([handle](Scene& scene)
			{
				Entity source(&scene, handle);
				if (source.IsValid() && !scene.IsPendingDestroy(handle))
					scene.DuplicateEntity(source);
			}))
			{
				MarkDocumentDirty();
			}
			else
				WLD_CORE_WARN("Duplicate entity request was rejected by the scene.");
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("Unable to duplicate entity: {0}", error.what());
		}
		catch (...)
		{
			WLD_CORE_ERROR("Unable to duplicate entity: unknown error.");
		}
	}

	void EditorLayer::ApplyRendererChange(const std::string& name)
	{
		if (!Renderer::SetRequestedRenderer(name))
			return;
		// 运行中热切换会串资源(GL 名字/描述符集跨上下文复用 → 图标与贴图错乱),
		// 改为自动重启编辑器进程:设置已写入清单,重启后即按新后端干净启动。
		RestartForRendererChange();
	}

	void EditorLayer::RestartForRendererChange()
	{
#ifdef WLD_PLATFORM_WINDOWS
		wchar_t exeBuffer[MAX_PATH] = {};
		if (GetModuleFileNameW(nullptr, exeBuffer, MAX_PATH) == 0)
		{
			WLD_CORE_ERROR("Renderer change requires restart, but the executable path is unknown.");
			return;
		}
		const std::filesystem::path exePath(exeBuffer);

		// 重启前把未保存的改动写回磁盘(切换渲染后端不应丢编辑内容)。
		if (m_Document.HasPath() && m_Document.IsDirty())
		{
			if (m_Document.SaveTo(m_Document.GetPath()))
				WLD_CORE_INFO("Saved scene before renderer restart: {0}", m_Document.GetPath().string());
			else
				WLD_CORE_WARN("Scene could not be saved before renderer restart: {0}",
					m_Document.GetLastError());
		}

		std::string arguments;
		if (m_Document.HasPath())
			arguments = " -scene \"" + m_Document.GetPath().string() + "\"";
		// 参数按 UTF-8 → UTF-16 转换(非 ASCII 路径不能按字节直接变宽字符)。
		std::wstring wideArguments;
		if (!arguments.empty())
		{
			const int wideLength = MultiByteToWideChar(CP_UTF8, 0, arguments.c_str(),
				static_cast<int>(arguments.size()), nullptr, 0);
			wideArguments.resize(static_cast<size_t>(std::max(0, wideLength)));
			if (wideLength > 0)
				MultiByteToWideChar(CP_UTF8, 0, arguments.c_str(), static_cast<int>(arguments.size()),
					wideArguments.data(), wideLength);
		}
		std::wstring commandLine = L"\"" + exePath.wstring() + L"\"" + wideArguments;

		// 场景路径同时通过环境块传给子进程(启动时即可见,且不受引号/编码影响)。
		std::wstring environmentBlock;
		if (m_Document.HasPath())
		{
			const std::wstring sceneWide = m_Document.GetPath().wstring();
			if (LPWCH current = GetEnvironmentStringsW())
			{
				for (const wchar_t* entry = current; *entry; entry += std::wcslen(entry) + 1)
				{
					if (std::wcsncmp(entry, L"WLD_START_SCENE=", 16) == 0)
						continue; // 覆盖旧值
					environmentBlock.append(entry);
					environmentBlock.push_back(L'\0');
				}
				FreeEnvironmentStringsW(current);
			}
			environmentBlock.append(L"WLD_START_SCENE=");
			environmentBlock.append(sceneWide);
			environmentBlock.push_back(L'\0');
			environmentBlock.push_back(L'\0');
		}

		DWORD lastError = 0;
		const auto tryLaunch = [&](LPVOID environment)
		{
			STARTUPINFOW startup {};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process {};
			std::wstring writableCommandLine = commandLine;   // CreateProcess 可能修改该缓冲
			const BOOL ok = CreateProcessW(exePath.wstring().c_str(), writableCommandLine.data(),
				nullptr, nullptr, FALSE, 0, environment, nullptr, &startup, &process);
			if (ok)
			{
				CloseHandle(process.hThread);
				CloseHandle(process.hProcess);
				return true;
			}
			lastError = GetLastError();
			return false;
		};

		// 优先沿用调用进程环境(-scene 参数已按 UTF-8→UTF-16 转换);
		// 自定义环境块作为第二选择(需要它时才构造额外环境)。
		bool launched = tryLaunch(nullptr);
		if (!launched && !environmentBlock.empty())
			launched = tryLaunch(environmentBlock.data());
		if (!launched)
		{
			// CreateProcess 可能被作业对象/权限策略拦下(错误码会写进日志);
			// 退一步交给 Shell 启动,它走 explorer 的令牌,通常不受当前进程作业限制。
			const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open", exePath.wstring().c_str(),
				wideArguments.empty() ? nullptr : wideArguments.c_str(), nullptr, SW_SHOWNORMAL);
			if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
			{
				const std::string message = "Renderer change requires a restart, but relaunching the editor failed"
					" (CreateProcess error " + std::to_string(lastError) + ", ShellExecute error " +
					std::to_string(static_cast<int>(reinterpret_cast<INT_PTR>(shellResult))) + ")."
					" The renderer setting has been saved; please restart the editor manually.";
				WLD_CORE_ERROR("{0}", message);
				ShowError(message);
				return;
			}
			WLD_CORE_INFO("Editor relaunched via ShellExecute (CreateProcess error {0})", lastError);
		}
		WLD_CORE_INFO("Editor restarted for renderer change (scene kept: {0})",
			m_Document.HasPath() ? m_Document.GetPath().string() : "(new scene)");
		Application::Get().Close();
#else
		WLD_CORE_WARN("Renderer change requires an editor restart on this platform.");
#endif
	}

	void EditorLayer::ProcessPendingRendererChange()
	{
		if (!m_RendererChangePending)
			return;
		m_RendererChangePending = false;

		// 在渲染开始前重建:上一帧的绘制命令已消费完,销毁旧设备/目标是安全的。
		if (m_SceneRenderer)
			m_SceneRenderer->Shutdown();
		Renderer::Shutdown();
		// GL 上下文与 Vulkan 表面不能在同一 HWND 上可靠共存(切回 GL 后呈现失效),
		// 因此换后端时重建主窗口(保留位置/尺寸/无边框/垂直同步)。
		Application::Get().RecreateWindow();
		Renderer::Init(m_RendererChangeName);
		m_Shell.RecreateIndependentWindows();
		// 窗口重建 = 新的 GL 上下文:重载旧式纹理并让面板(内容浏览器等)也重载。
		++m_TextureEpoch;
		LoadIconTextures();
		if (m_SceneRenderer)
			m_SceneRenderer->Init();
		WLD_CORE_INFO("[switch] scene renderer rebuilt for {0}", Renderer::GetBackendName());
		RegisterUiTextures();
		WLD_CORE_INFO("[switch] ui textures registered");

		m_ViewportSize = { 0, 0 };
		WLD_CORE_INFO("Editor renderer switched to {0}", Renderer::GetBackendName());
	}

	void EditorLayer::RegisterUiTextures()
	{
		auto& registry = Wui::WuiTextureRegistry::Get();
		if (m_SceneRenderer && m_SceneRenderer->GetColorTexture())
			m_SceneTextureId = registry.Register(m_SceneRenderer->GetColorTexture());
		const Ref<Texture2D> icons[8] = {
			m_IconPlay, m_IconStop, m_IconPause, m_IconContinue,
			m_IconSimulate, m_IconSimulateStop, m_IconSimulatePause, m_IconSimulateContinue,
		};
		for (int i = 0; i < 8; ++i)
			m_IconIds[i] = registry.RegisterTexture2D(icons[i]);
		m_UiTextureGeneration = registry.Generation();
	}

	uint64_t EditorLayer::GetIconId(int index) const
	{
		if (index < 0 || index >= 8)
			return 0;
		return m_IconIds[index];
	}

	void EditorLayer::TogglePlay()
	{
		SetSceneState(m_SceneState == SceneState::Play ? SceneState::Edit : SceneState::Play);
	}

	void EditorLayer::ToggleSimulate()
	{
		SetSceneState(m_SceneState == SceneState::Simulate ? SceneState::Edit : SceneState::Simulate);
	}

	void EditorLayer::TogglePause()
	{
		if (m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate)
			m_ScenePaused = !m_ScenePaused;
	}

	Ref<Texture2D> EditorLayer::GetIcon(int index) const
	{
		switch (index)
		{
			case 1: return m_IconStop;
			case 2: return m_IconPause;
			case 3: return m_IconContinue;
			case 4: return m_IconSimulate;
			case 5: return m_IconSimulateStop;
			case 6: return m_IconSimulatePause;
			case 7: return m_IconSimulateContinue;
			default: return m_IconPlay;
		}
	}

	void EditorLayer::SetViewportState(bool focused, bool hovered, glm::vec2 size, glm::vec2 bounds[2])
	{
		m_ViewportFocused = focused;
		m_ViewportHovered = hovered;
		m_ViewportBounds[0] = bounds[0];
		m_ViewportBounds[1] = bounds[1];
		if (size.x > 0 && size.y > 0 && (m_ViewportSize.x != size.x || m_ViewportSize.y != size.y))
		{
			// 相机宽高比即时跟随,渲染目标延迟重建(见 OnUpdate)。
			// 注意:节流窗口只在**尺寸变化时**重置。每帧无条件重置会让它永远不到期
			// (帧间隔 < 0.1s 时,m_ViewportSize 又是在到期后才更新 → 死循环,
			// 表现是 GL 无 VSync 下视口目标从不重建、尺寸永远停在初始值)。
			if (m_PendingViewportSize != size)
			{
				m_PendingViewportSize = size;
				m_ViewportResizeDelay = 0.1f;
			}
			m_EditorCamera.SetViewportSize(size.x, size.y);
			m_EditorCamera3D.SetViewportSize(static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y));
		}

	}

	void EditorLayer::ResolveUnsavedModal(bool save)
	{
		std::function<void()> action = std::move(m_PendingAction);
		m_PendingAction = nullptr;
		m_ShowUnsavedModal = false;
		if (save && !TrySave())
		{
			if (!m_Document.GetLastError().empty())
				WLD_CORE_ERROR("{0}", m_Document.GetLastError());
			return;
		}
		if (action)
			action();
	}

	void EditorLayer::CancelUnsavedModal()
	{
		m_PendingAction = nullptr;
		m_ShowUnsavedModal = false;
	}

	bool EditorLayer::TrySave()
	{
		m_Document.ClearError();
		if (!m_Document.HasPath())
		{
			std::string path = FileDialogs::SaveFile("Scene File (*.wd)\0*.wd\0");
			if (path.empty())
				return false;
			return m_Document.SaveTo(std::filesystem::path(path));
		}
		return m_Document.SaveTo(m_Document.GetPath());
	}
	bool EditorLayer::OnKeyPressed(KeyPressedEvent& e)
	{
		if (e.GetRepeatCount() > 0)
			return false;

		bool control = Input::IsKeyPressed(KeyCodes::LeftControl) || Input::IsKeyPressed(KeyCodes::RightControl);
		bool shift = Input::IsKeyPressed(KeyCodes::LeftShift) || Input::IsKeyPressed(KeyCodes::RightShift);


		return m_Commands.HandleKey(e.GetKeyCode(), control, shift);
	}
	Entity EditorLayer::GetEntityAtMousePosition(glm::vec2 viewportLocal)
	{
		if (!m_HasRenderedScene || !m_ActiveScene || !m_SceneRenderer)
			return {};
		// --- 鼠标交互逻辑 ---

		// 获取鼠标在屏幕上的绝对位置
		glm::vec2 viewportSizeAvail = { m_ViewportBounds[1].x - m_ViewportBounds[0].x, m_ViewportBounds[1].y - m_ViewportBounds[0].y };
		if (viewportSizeAvail.x <= 0.0f || viewportSizeAvail.y <= 0.0f)
			return {};

		// D7-1c:拾取走 SceneRenderer 的后端无关读回(左上角原点,内部处理 GL/VK 行序),
		// 不再依赖 Framebuffer::ReadPixel —— 那条只有 OpenGL 实现,Vulkan 下拾取是坏的。
		// 视口尺寸与渲染目标尺寸理论上一致,这里按比例换算以防两侧短暂不同步(拖分隔条)。
		const float scaleX = static_cast<float>(m_SceneRenderer->GetWidth()) / viewportSizeAvail.x;
		const float scaleY = static_cast<float>(m_SceneRenderer->GetHeight()) / viewportSizeAvail.y;
		const int mouseX = static_cast<int>(viewportLocal.x * scaleX);
		const int mouseY = static_cast<int>(viewportLocal.y * scaleY);

		int pixelData = -1;
		// 边界检查：只有当鼠标在黑色内容区内时才读取
		if (mouseX >= 0 && mouseY >= 0 && mouseX < (int)m_SceneRenderer->GetWidth() && mouseY < (int)m_SceneRenderer->GetHeight())
			pixelData = m_SceneRenderer->ReadEntityIdAt(mouseX, mouseY);
		if (std::getenv("WLD_TRACE_UI"))
			WLD_CORE_INFO("[ui] pick viewport=({0},{1}) texel=({2},{3}) -> id={4}",
				viewportLocal.x, viewportLocal.y, mouseX, mouseY, pixelData);

		Entity result = pixelData == -1 ? Entity() : Entity(m_ActiveScene.get(), (entt::entity)pixelData);

		return result.IsValid() && !m_ActiveScene->IsPendingDestroy(result) ? result : Entity{};
	}
	void EditorLayer::SetSceneState(SceneState state)
	{
		if (m_SceneState == state) return;

		// End the previous mode before replacing any scene references.
		if (m_RuntimeScene)
		{
			if (m_SceneState == SceneState::Play)
			{
				// Play 的运行时生命周期由 PlayHost(GameApp 会话)持有,不能在外部直接 StopScene。
				m_PlayHost.StopRuntime();
				m_PlayHost.Shutdown();
			}
			else if (m_SceneState == SceneState::Simulate)
				m_RuntimeScene->OnSimulationStop();
		}
		m_SceneState = SceneState::Edit;
		m_ScenePaused = false;
		UpdateSceneContext(m_Document.GetScene());
		m_RuntimeScene.reset();
		if (state == SceneState::Edit || !m_Document.GetScene())
			return;

		try
		{
			m_RuntimeScene = CreateRef<Scene>(Application::Get().GetContext());
			Ref<Scene> editorScene = m_Document.GetScene();
			Scene::CopyScene(editorScene, m_RuntimeScene);
			m_SceneState = state;
			if (state == SceneState::Play)
			{
				// Play:接入 GameApp 会话(渲染器注入编辑器视图口渲染器,启动运行时)。
				Gameplay::GameAppDesc desc;
				desc.ProjectId = "worldengine-editor-play";
				desc.FixedStepHz = 60;
				m_PlayHost.Init(desc);
				m_PlayHost.SetRenderer(m_SceneRenderer);
				m_PlayHost.SetScene(m_RuntimeScene, /*startRuntime=*/true);
			}
			else
				m_RuntimeScene->OnSimulationStart();
			// 视口尺寸在 SetScene 之后覆盖:GameHost 默认按宿主窗口同步,编辑器要用视图口尺寸。
			UpdateSceneContext(m_RuntimeScene);
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("Unable to start scene: {0}", error.what());
			if (m_RuntimeScene)
				m_RuntimeScene->OnRuntimeStop();
			UpdateSceneContext(m_Document.GetScene());
			m_RuntimeScene.reset();
			m_SceneState = SceneState::Edit;
		}
		catch (...)
		{
			WLD_CORE_ERROR("Unable to start scene: unknown error.");
			if (m_RuntimeScene)
				m_RuntimeScene->OnRuntimeStop();
			UpdateSceneContext(m_Document.GetScene());
			m_RuntimeScene.reset();
			m_SceneState = SceneState::Edit;
		}
	}
	void EditorLayer::UpdateSceneContext(Ref<Scene> scene)
	{
		m_HasRenderedScene = false;
		m_ActiveScene = scene;
		if (m_ActiveScene && m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f)
		{
			m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		}
	}

	void EditorLayer::RequestAction(std::function<void()> action)
	{
		if (m_Document.IsDirty())
		{
			m_PendingAction = std::move(action);
			m_ShowUnsavedModal = true;
			return;
		}
		action();
	}

	void EditorLayer::ShowError(const std::string& message)
	{
		m_ErrorText = message;
		m_ShowErrorModal = true;
		WLD_CORE_ERROR("{0}", message);
	}



	void EditorLayer::StartCooking(const std::string& target)
	{
		if (m_CookingThread.joinable())
		{
			if (!m_CookingFinished.load(std::memory_order_acquire))
				return;
			m_CookingThread.join();
		}
		m_ShowCookingProgress = true;
		m_CookingSucceeded = false;
		m_CookingError.clear();
		m_CookingFinished.store(false, std::memory_order_relaxed);
		try
		{
			m_CookingThread = std::thread([target, this]()
			{
				namespace fs = std::filesystem;
				Editor::CookOptions options;
				options.PublishDir = target;

				// 当前打开场景若在内容根内,覆盖清单里的启动场景。
				if (m_Document.HasPath())
				{
					std::string manifestError;
					World::Asset::ProjectManifest manifest;
					const fs::path projectManifestPath = std::string(WLD_GAME_DIR) + "project.we.yaml";
					if (World::Asset::ProjectManifest::Load(projectManifestPath, &manifest, &manifestError))
					{
						const fs::path contentRoot = manifest.ResolveContentRoot(projectManifestPath);
						const fs::path relative = fs::relative(m_Document.GetPath(), contentRoot);
						if (!relative.empty() && relative.generic_string().find("..") == std::string::npos)
							options.StartSceneOverride = relative;
					}
				}

				const Editor::CookResult cooked = Editor::CookProject(options);
				m_CookingError = cooked.Error;
				m_CookingSucceeded = cooked.Ok;
				m_CookingFinished.store(true, std::memory_order_release);
			});
		}
		catch (const std::exception& error)
		{
			m_CookingError = error.what();
			m_CookingFinished.store(true, std::memory_order_release);
			WLD_CORE_ERROR("Unable to start cooking: {0}", m_CookingError);
		}
	}



}


