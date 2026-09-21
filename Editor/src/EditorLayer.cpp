#include "EditorLayer.h"
#include "EditorCooker.h"
#include "EditorPreferences.h"
#include "EditorResources.h"
#include "EditorStartup.h"
#include "World/Core/Asset/BuiltinImporters.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/Core/Asset/GltfImporter.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Core/Vfs/DirectoryProvider.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Gameplay/ModelInstance.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/HotReload.h"
#include "World/WUI/WuiRhiBackend.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/WuiScriptedInput.h"
#include "World/WUI/WuiAccessibility.h"
#include <filesystem>
#include <shellapi.h>
#include <stdexcept>
#include <chrono>
#include <algorithm>
#include "World/Events/MouseEvent.h"
namespace World
{
	namespace
	{
		// WLD_FRAME_TIMING=1:把编辑器一帧拆成"场景更新 / AI+WUI 起帧 / 面板逻辑 / WUI 录制提交"
		// 四段(默认关;只做诊断,不改变行为)。配合 Renderer 的 [frame-timing] 使用:
		// Renderer 那条覆盖交换链/present,这条覆盖层栈内部。
		struct LayerTiming
		{
			bool Enabled = false;
			double Update = 0, UiFrame = 0, Begin = 0, Shell = 0, WuiRender = 0;
			uint32_t Frames = 0;
		};

		LayerTiming& LayerTimingState()
		{
			static LayerTiming timing = [] {
				LayerTiming out;
				const char* env = std::getenv("WLD_FRAME_TIMING");
				out.Enabled = env && env[0] != '\0' && env[0] != '0';
				return out;
			}();
			return timing;
		}

		double LayerTimingNowMs()
		{
			return std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		struct LayerTimingScope
		{
			explicit LayerTimingScope(double& sink) : m_Sink(&sink), m_Start(0.0)
			{
				if (LayerTimingState().Enabled)
					m_Start = LayerTimingNowMs();
			}
			~LayerTimingScope()
			{
				if (m_Sink && LayerTimingState().Enabled)
					*m_Sink += LayerTimingNowMs() - m_Start;
			}
			double* m_Sink;
			double m_Start;
		};

		void LayerTimingFlush()
		{
			LayerTiming& timing = LayerTimingState();
			if (!timing.Enabled || ++timing.Frames < 120)
				return;
			const double frames = static_cast<double>(timing.Frames);
			WLD_CORE_INFO("[layer-timing] update={0:.2f} uiFrame={1:.2f} (begin={2:.2f} shell={3:.2f} wuiRender={4:.2f}) "
				"ms/frame, n={5}",
				timing.Update / frames, timing.UiFrame / frames, timing.Begin / frames,
				timing.Shell / frames, timing.WuiRender / frames, timing.Frames);
			timing.Update = timing.UiFrame = timing.Begin = timing.Shell = timing.WuiRender = 0.0;
			timing.Frames = 0;
		}
	}

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
		// AI 控制通道:只有显式 --ai-control=<port> 时才监听(默认关闭,零行为变化)。
		if (const int aiPort = Editor::AiControlPort(); aiPort > 0)
		{
			m_AiServer = std::make_unique<Editor::AiControlServer>();
			if (!m_AiServer->Start(static_cast<uint16_t>(aiPort),
				[this](const std::string& cmd, const std::map<std::string, std::string>& args,
					std::string& result, std::string& error)
				{
					const bool ok = ExecuteAiCommand(cmd, args, result, error);
					if (ok)
						AiRecordCommand(cmd, args);
					return ok;
				}))
			{
				WLD_CORE_ERROR("[ai] failed to start control channel on port {0}", aiPort);
				m_AiServer.reset();
			}
			else
				// 无障碍树只在通道开启时登记(正常编辑零开销)。
				Wui::WuiAccessibility::Get().SetEnabled(true);
		}
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

		// W8:Luau LSP 脚手架(.vscode/settings.json + .luau-lsp/config.json):create-if-missing,
		// 磁盘已有(用户改过的)配置绝不覆盖。
		{
			// 工作区根 = 项目清单所在目录(Game/;清单里的 content_root = assets),与
			// 入库的 Game/.vscode、Game/.luau-lsp 一致;没有清单时退回内容根。
			std::filesystem::path scaffoldRoot = std::filesystem::path(WLD_ASSETPATH);
			std::filesystem::path manifestPath;
			if (World::Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
				scaffoldRoot = manifestPath.parent_path();
			std::string scaffoldError;
			if (!EnsureScriptEditorScaffold(scaffoldRoot, &scaffoldError))
				WLD_CORE_ERROR("Luau LSP scaffold creation failed: {0}", scaffoldError);
		}

		m_SceneRenderer = CreateRef<SceneRenderer>();
		m_SceneRenderer->Init();
		// 相机可视化:预览目标(独立小尺寸 SceneRenderer,与主视口同一条提交路径)。
		{
			if (const char* previewEnv = std::getenv("WLD_CAMERA_PREVIEW"))
				m_CameraPreviewEnabled = std::atoi(previewEnv) != 0;
			uint32_t previewWidth = 480, previewHeight = 270;
			if (const char* sizeEnv = std::getenv("WLD_CAMERA_PREVIEW_SIZE"))
			{
				unsigned int w = 0, h = 0;
				if (std::sscanf(sizeEnv, "%ux%u", &w, &h) == 2 && w >= 32 && h >= 32)
				{
					previewWidth = w;
					previewHeight = h;
				}
			}
			m_PreviewRenderer = CreateRef<SceneRenderer>();
			m_PreviewRenderer->Init();
			m_PreviewRenderer->OnResize(previewWidth, previewHeight);
		}

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
				{
					projectId = manifest.Id;
					// D8a2:项目级渲染设置(rendering.*)随清单一起生效;渲染器 Init 也会
					// 自己装载一次,这里补上"运行中重新加载项目清单"的路径。
					World::RenderSettings::Apply(manifest);
				}
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
		// D5c-5:从 3D 档起步(自动化入口)时同样先对齐 2D 视角,避免"从背面看场景"。
		if (m_Viewport3D)
			AlignEditorCamera3DWithView();

		// 开发验证:WLD_HIERARCHY_DEMO=1 现场造一个"父精灵 + 子精灵"的最小层级
		// (子实体局部偏移 0.4):验证 2D 精灵子节点跟随父节点 —— 渲染路径必须消费
		// WorldTransformComponent,而不是子实体自己的局部矩阵。
		// 开发验证:WLD_HIERARCHY_DEMO=1 把场景里名为 "Sprite B" 的实体挂到 "Sprite A" 下
		// (B 的局部变换保持不变)。用于验证 2D 精灵子节点跟随父节点:
		// 修复前 B 画在局部位置(画面中央),修复后画在 A+B 的世界位置(A 的左侧)。
		if (std::getenv("WLD_HIERARCHY_DEMO") && m_ActiveScene)
		{
			Entity parent, child;
			const auto& registry = static_cast<const Scene*>(m_ActiveScene.get())->GetRegistry();
			for (const entt::entity handle : registry.view<TagComponent>())
			{
				const std::string& tag = registry.get<TagComponent>(handle).Tag;
				if (tag == "Sprite A")
					parent = Entity(m_ActiveScene.get(), handle);
				else if (tag == "Sprite B")
					child = Entity(m_ActiveScene.get(), handle);
			}
			if (child.IsValid() && parent.IsValid())
			{
				m_ActiveScene->DeferStructuralChange([&child, &parent](Scene& scene)
				{
					Hierarchy::SetParent(scene.GetRegistry(), static_cast<entt::entity>(child),
						static_cast<entt::entity>(parent));
				});
				WLD_CORE_INFO("[dev] hierarchy demo: '{0}' (handle={1}) is now a child of '{2}' (handle={3})",
					"Sprite B", static_cast<uint32_t>(static_cast<entt::entity>(child)),
					"Sprite A", static_cast<uint32_t>(static_cast<entt::entity>(parent)));
			}
			else
			{
				// 兜底:按句柄取前两个精灵(2DTest 的 A/B 就是 0/1)。标签查询在
				// 场景刚反序列化时可能还没填好,这里保证验证脚本总能成立。
				int found = 0;
				for (const entt::entity handle : registry.view<SpriteComponent>())
				{
					if (found == 0)
						parent = Entity(m_ActiveScene.get(), handle);
					else if (found == 1)
						child = Entity(m_ActiveScene.get(), handle);
					if (++found >= 2)
						break;
				}
				if (child.IsValid() && parent.IsValid())
				{
					m_ActiveScene->DeferStructuralChange([&child, &parent](Scene& scene)
					{
						Hierarchy::SetParent(scene.GetRegistry(), static_cast<entt::entity>(child),
							static_cast<entt::entity>(parent));
					});
					WLD_CORE_INFO("[dev] hierarchy demo (by handle): child={0} -> parent={1}",
						static_cast<uint32_t>(static_cast<entt::entity>(child)),
						static_cast<uint32_t>(static_cast<entt::entity>(parent)));
				}
				else
					WLD_CORE_WARN("[dev] hierarchy demo: fewer than two sprites in the scene");
			}
		}
	}

	// 图标是旧式(GL)纹理:窗口/上下文重建后必须重新加载,否则渲染出的图标会错乱。
	void EditorLayer::LoadIconTextures()
	{
		// 图标是**编辑器资源**(Editor/Resource/Icons),不是内容根里的游戏资产 ——
		// 一律走 EditorResourcePath 拼绝对路径(见 EditorResources.h)。
		m_IconPlay = Texture2D::Create(EditorResourcePath("Resource/Icons/Icon_Play.png"));

		m_IconStop = Texture2D::Create(EditorResourcePath("Resource/Icons/Icon_Stop.png"));

		m_IconPause = Texture2D::Create(EditorResourcePath("Resource/Icons/Icon_Pause.png"));
		m_IconContinue = Texture2D::Create(EditorResourcePath("Resource/Icons/Icon_Continue.png"));

		m_IconSimulate = Texture2D::Create(EditorResourcePath("Resource/Icons/Icon_SimulateStart.png"));
		m_IconSimulateStop = Texture2D::Create(EditorResourcePath("Resource/Icons/Icon_SimulateStop.png"));
		m_IconSimulatePause = Texture2D::Create(EditorResourcePath("Resource/Icons/Icon_SimulatePause.png"));
		m_IconSimulateContinue = Texture2D::Create(EditorResourcePath("Resource/Icons/Icon_SimulateContinue.png"));
	}

	void EditorLayer::OnDetach()
	{
		WLD_PROFILE_FUNCTION();
		// 控制通道先停:避免关停过程中还有命令进来(Pump 已经不会再被调用)。
		if (m_AiServer)
		{
			m_AiServer->Stop();
			m_AiServer.reset();
		}
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
		if (m_PreviewRenderer)
		{
			m_PreviewRenderer->Shutdown();
			m_PreviewRenderer.reset();
		}
		// 独立窗口(含附加状态)必须先于 RHI 设备/主窗口销毁,否则关闭引擎时会崩。
		m_Shell.ReleaseIndependentWindows();
		ExportOperationLog();
	}

	void EditorLayer::OnUpdate(Timestep ts)
	{
		WLD_PROFILE_FUNCTION();
		LayerTimingScope updateScope(LayerTimingState().Update);
		// D5c-4a:渲染发生在面板绘制里(RenderScene),那里拿不到 Timestep —— 先缓存一帧。
		m_LastDeltaSeconds = ts.GetSeconds();
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
		// P2 W5b:帧边界(不在任何脚本回调内)轮询脚本热重载。编辑态轮询文档场景,
		// Play/Simulate 轮询正在跑的那个副本 —— 改盘即生效,用户当场看到结果。
		PollScriptHotReload(ts.GetSeconds());
		// P2 W5-L1:同一帧边界轮询资产外部改动(材质/贴图自动;文档场景只提示)。
		PollAssetHotReload(ts.GetSeconds());
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
		// 开发/验证钩子:WLD_AUTOSIMULATE=<帧数> 时在该帧自动进入 Simulate(等价于点视图口
		// Simulate 按钮)。P2 W4 起 Simulate 也走 GameApp/GameHost 会话,这里把"会话是否真的
		// 建立"和当前场景状态一起打进日志,供隐藏冒烟断言。
		if (const char* autoSimulateFrames = std::getenv("WLD_AUTOSIMULATE"))
		{
			static int devSimulateFrame = 0;
			static bool devAutoSimulateDone = false;
			const int target = std::atoi(autoSimulateFrames);
			if (!devAutoSimulateDone && target > 0 && ++devSimulateFrame >= target)
			{
				devAutoSimulateDone = true;
				ToggleSimulate();
				WLD_CORE_INFO("[dev] WLD_AUTOSIMULATE: entered Simulate after {0} frames (scene state {1}, GameApp session {2})",
					devSimulateFrame, static_cast<int>(m_SceneState),
					Gameplay::GameApp::Exists() ? "active" : "missing");
			}
		}
		RunHierarchyClickCheck();
		// 开发验证:WLD_AUTOPAUSE=<进入 Play 后的帧数> 在该帧自动暂停(验证 Play 暂停态的
		// 覆盖层/相机切换:暂停时渲染会用回编辑器相机,见本函数末尾的相机分支)。
		if (const char* autoPauseFrames = std::getenv("WLD_AUTOPAUSE"))
		{
			static int devPauseCounter = 0;
			static bool devAutoPauseDone = false;
			const int target = std::atoi(autoPauseFrames);
			if (!devAutoPauseDone && m_SceneState == SceneState::Play && target > 0 && ++devPauseCounter >= target)
			{
				devAutoPauseDone = true;
				TogglePause();
				WLD_CORE_INFO("[dev] WLD_AUTOPAUSE: paused after {0} Play frames", devPauseCounter);
			}
		}
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
				case SceneState::Simulate:
				{
					// 与 Runtime 同一条路径:GameApp 驱动阶段回调(GameHost 内注册的 update
					// 会调用场景 OnUpdateRuntime);渲染仍由编辑器视图口统一提交(render=false)。
					// P2 W4:Simulate 并入同一条会话路径(事件/计时器/输入/关卡随之生效);
					// 暂停时仍 Tick 会话,让 queued 事件在暂停下也投递,而固定步长/计时器由
					// GameApp 的暂停标志冻结(见 GameApp::Tick 契约)。
					m_PlayHost.Tick(ts, /*render=*/false);
					if (m_ScenePaused)
						m_ActiveScene->FlushStructuralChanges();
					break;
				}
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
		// D5c-4a:骨骼动画步长(编辑态也推进,便于在视口里直接看动画;Play 时同一口径)。
		m_SceneRenderer->SetDeltaSeconds(m_LastDeltaSeconds);
		m_SceneRenderer->SubmitScene(*renderCamera, renderCameraTransform, selectedEntity);
		m_SceneRenderer->EndScene();
		// 相机可视化:同一帧再给"场景相机"渲一份小图(PiP)。Edit/Simulate 才有意义 ——
		// Play 时主视口本身就是这台相机。
		if (m_CameraPreviewEnabled && m_SceneState != SceneState::Play)
			RenderCameraPreview();
		m_HasRenderedScene = true;
		// 开发验证:像素基线截图(与 Runtime 同名开关)。走的是后端无关的 RHI 读回,
		// Vulkan/GL 都能抓到本帧场景颜色附件。
		CaptureFrameIfRequested();
		// 点选校验必须在场景渲染之后:m_HasRenderedScene 在 OnUpdate 开头被复位,
		// 放在前面会让 GetEntityAtMousePosition 直接早退(等于没测)。
		RunPickCheck();
		// 开发验证:WLD_SELECT_HANDLE=<句柄> 在渲染稳定后直接选中该实体(不退出),
		// 供"选中框/描边"这类 UI 覆盖层的自动化核对使用。
		if (const char* selectEnv = std::getenv("WLD_SELECT_HANDLE"))
		{
			static bool s_Selected = false;
			// WLD_SELECT_IN_PLAY=1 时等 Play 起来再选(验证 Play 下的覆盖层/HUD 坐标)。
			const bool waitForPlay = std::getenv("WLD_SELECT_IN_PLAY") != nullptr;
			const bool ready = waitForPlay ? (m_SceneState == SceneState::Play) : (m_SceneState == SceneState::Edit);
			if (!s_Selected && ready && m_ActiveScene)
			{
				s_Selected = true;
				Entity target;
				// 支持 WLD_SELECT_HANDLE=camera:直接选中场景主相机(相机可视化验收用)。
				if (std::strcmp(selectEnv, "camera") == 0)
					target = m_ActiveScene->GetPrimaryCameraEntity();
				else
					target = Entity(m_ActiveScene.get(), static_cast<entt::entity>(std::atoi(selectEnv)));
				if (!target.IsValid() && std::strcmp(selectEnv, "camera") != 0)
				{
					// 句柄在当前场景不存在(Play 用播放副本):退化为第一个网格实体。
					const entt::registry& registry = static_cast<const Scene*>(m_ActiveScene.get())->GetRegistry();
					for (const entt::entity entity : registry.view<TransformComponent, MeshRendererComponent>())
					{
						target = Entity(m_ActiveScene.get(), entity);
						break;
					}
				}
				m_SelectedEntity = target;
				WLD_CORE_INFO("[dev] select handle={0} play={1} -> valid={2} handle={3}",
					std::atoi(selectEnv), m_SceneState == SceneState::Play ? 1 : 0,
					m_SelectedEntity.IsValid() ? 1 : 0,
					m_SelectedEntity.IsValid() ? static_cast<uint32_t>(static_cast<entt::entity>(m_SelectedEntity)) : 0u);
			}
		}
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
		if (pathEnv && pathEnv[0] && m_SceneRenderer)
		{
			WLD_CORE_INFO("[capture] scene target {0}x{1}, viewport {2}x{3}",
				m_SceneRenderer->GetWidth(), m_SceneRenderer->GetHeight(),
				static_cast<uint32_t>(m_ViewportSize.x), static_cast<uint32_t>(m_ViewportSize.y));
			m_SceneRenderer->CaptureFrame(pathEnv);
		}
		// 相机预览目标(相机可视化验收:预览图必须与"该相机看到的画面"一致)。
		if (const char* previewPath = std::getenv("WLD_CAPTURE_PREVIEW"))
		{
			// 只有"确实在预览某台相机"时才写文件:未选中相机时预览目标里是空内容,
			// 抓出来会误导(验收脚本据此判断"有没有预览")。
			if (previewPath[0] && m_PreviewRenderer && m_CameraPreviewEnabled &&
				GetPreviewCameraEntity().IsValid())
			{
				WLD_CORE_INFO("[capture] camera preview target {0}x{1}",
					m_PreviewRenderer->GetWidth(), m_PreviewRenderer->GetHeight());
				m_PreviewRenderer->CaptureFrame(previewPath);
			}
		}
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
			// WLD_PICK_SELFTEST=1:不依赖硬编码坐标 —— 先扫描 entity 附件找出每个可见实体,
			// 再对每个实体取一个**内部**像素走正常拾取路径,验证拿回的句柄与附件一致。
			// 这样布局/场景变化都不会让验收失效(用户要求"点方块任意位置应该选中")。
			m_DevPickSelfTest = std::getenv("WLD_PICK_SELFTEST") != nullptr;
			const char* spec = m_DevPickSelfTest ? nullptr : std::getenv("WLD_PICK_AT");
			if ((!spec || !spec[0]) && !m_DevPickSelfTest)
			{
				m_DevPickFrames = -2; // 未启用
				return;
			}
			const char* framesEnv = std::getenv("WLD_PICK_FRAMES");
			m_DevPickFrames = framesEnv ? std::atoi(framesEnv) : 30;
			if (m_DevPickFrames < 3)
				m_DevPickFrames = 3;
			if (m_DevPickSelfTest)
			{
				m_DevPickFrames = m_DevPickFrames < 3 ? 3 : m_DevPickFrames;
				return;
			}
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

		if (m_DevPickSelfTest)
		{
			std::vector<int32_t> ids;
			if (!m_SceneRenderer->ReadEntityIdBuffer(ids))
			{
				WLD_CORE_ERROR("[dev] pick self-test: FAIL (entity attachment readback failed)");
				Application::Get().Close();
				return;
			}
			const int32_t width = static_cast<int32_t>(m_SceneRenderer->GetWidth());
			const int32_t height = static_cast<int32_t>(m_SceneRenderer->GetHeight());
			// P4-3:附件 texel 坐标 → 视口局部坐标。上面扫描用的是**渲染目标**像素
			// (GetWidth/GetHeight = 请求尺寸 × rendering.render_scale),而
			// GetEntityAtMousePosition 收的是视口坐标;render_scale ≠ 1 时两者差一个
			// "请求尺寸 / 目标尺寸"倍率。倍率 1.0 时两尺寸相等、乘数为 1.0f,
			// 行为与旧代码逐字节一致(同一条拾取路径)。
			const float texelToViewportX =
				static_cast<float>(m_SceneRenderer->GetRequestedWidth()) / static_cast<float>(width);
			const float texelToViewportY =
				static_cast<float>(m_SceneRenderer->GetRequestedHeight()) / static_cast<float>(height);
			// 每个实体取一个"内部"像素(四邻同 id):轮廓边缘 1px 可能因边界效应判空。
			std::map<int32_t, glm::ivec2> sample;
			for (int32_t y = 1; y < height - 1 && sample.size() < 64; ++y)
				for (int32_t x = 1; x < width - 1; ++x)
				{
					const int32_t id = ids[static_cast<size_t>(y) * width + x];
					if (id == -1 || sample.count(id))
						continue;
					if (ids[static_cast<size_t>(y) * width + x - 1] == id &&
						ids[static_cast<size_t>(y) * width + x + 1] == id &&
						ids[static_cast<size_t>(y - 1) * width + x] == id &&
						ids[static_cast<size_t>(y + 1) * width + x] == id)
						sample[id] = { x, y };
				}
			int checked = 0;
			int failed = 0;
			for (const auto& [id, point] : sample)
			{
				const Entity picked = GetEntityAtMousePosition({ static_cast<float>(point.x) * texelToViewportX,
					static_cast<float>(point.y) * texelToViewportY });
				const int32_t pickedId = picked.IsValid()
					? static_cast<int32_t>(static_cast<uint32_t>(static_cast<entt::entity>(picked))) : -1;
				const bool ok = pickedId == id;
				++checked;
				if (!ok)
					++failed;
				WLD_CORE_INFO("[dev] pick self-test: entity id={0} at ({1},{2}) -> picked={3} {4}",
					id, point.x, point.y, pickedId, ok ? "PASS" : "FAIL");
			}
			WLD_CORE_INFO("[dev] pick self-test: {0} (checked={1} failed={2})",
				failed == 0 && checked > 0 ? "PASS" : "FAIL", checked, failed);
			Application::Get().Close();
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
		LayerTimingScope uiFrameScope(LayerTimingState().UiFrame);
		// 手动打点(三段之间夹着"面板逻辑"与"WUI 录制",各自还要在末尾一次性累加)。
		const bool timingEnabled = LayerTimingState().Enabled;
		const double uiStart = timingEnabled ? LayerTimingNowMs() : 0.0;

		static Wui::WuiRhiBackend wuiBackend;

		// 多窗口:每帧开始前显式把主窗口的 GL 上下文设为当前,
		// 避免上一帧独立窗口渲染留下的上下文影响主窗口的绘制与交换。
		Application::Get().GetWindow().MakeCurrent();

		// AI 控制通道:命令在主线程帧内执行(与鼠标操作同一顺序);随后把排队的脚本点击
		// 注入到本窗口的输入状态 —— 控件侧看到的仍是普通输入,不是测试专用分支。
		if (m_AiServer)
			m_AiServer->Pump();

		Wui::WuiInputState input;
		if (wuiBackend.BeginFrame(input))
		{
			Wui::WuiScriptedInput::Get().Apply("main", input);
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
			// 相机预览小窗的纹理(与主场景纹理同样按纪元重注册)。
			if (m_CameraPreviewEnabled && m_PreviewRenderer && m_PreviewRenderer->GetColorTexture())
			{
				if (!m_PreviewTextureId)
					m_PreviewTextureId = Wui::WuiTextureRegistry::Get().Register(m_PreviewRenderer->GetColorTexture());
				else
					Wui::WuiTextureRegistry::Get().Update(m_PreviewTextureId, m_PreviewRenderer->GetColorTexture());
			}
			// 诊断:模拟"用户点开材质编辑器"(与内容浏览器同一条 OpenMaterialEditor 路径),
			// 用于区分"启动时由布局恢复打开"与"运行期打开"两种场景的行为差异。
			//   WLD_OPEN_MATERIAL_AT=<帧号> WLD_OPEN_MATERIAL_PATH=<材质路径>
			{
				static const char* openAt = std::getenv("WLD_OPEN_MATERIAL_AT");
				static const char* openPath = std::getenv("WLD_OPEN_MATERIAL_PATH");
				if (openAt && *openAt && openPath && *openPath)
				{
					static int openFrame = 0;
					if (++openFrame == std::atoi(openAt))
					{
						WLD_CORE_INFO("[diag] open material editor at frame {0}: {1}", openFrame, openPath);
						m_Shell.OpenMaterialEditor(openPath);
					}
				}
			}
			const double beginEnd = timingEnabled ? LayerTimingNowMs() : 0.0;
			m_Shell.OnRender(m_WuiContext);
			m_WuiContext.EndFrame();
			const double shellEnd = timingEnabled ? LayerTimingNowMs() : 0.0;
			wuiBackend.Render(m_WuiContext.Commands(), m_WuiContext.OverlayCommands());
			// AI 控制通道的整窗抓图:UI 已提交、尚未做 →Present 布局转换/呈现。
			Renderer::FlushPresentCaptures();
			if (timingEnabled)
			{
				LayerTiming& timing = LayerTimingState();
				timing.Begin += beginEnd - uiStart;
				timing.Shell += shellEnd - beginEnd;
				timing.WuiRender += LayerTimingNowMs() - shellEnd;
			}
		}
		// 屏幕快照钩子(诊断无障碍化):把**用户实际看到的整个窗口**连续写成 PPM,
		// 用于自动化诊断"闪烁"这类只在最终画面里可见的问题。
		//   WLD_SCREEN_CAPTURE_DIR=<目录>   输出目录
		//   WLD_SCREEN_CAPTURE_START=<帧号>  从第几帧开始(默认 0,配合 WLD_CAPTURE_DELAY 无意义时用)
		//   WLD_SCREEN_CAPTURE_EVERY=<n>    每 n 帧抓一张(默认 1)
		//   WLD_SCREEN_CAPTURE_COUNT=<n>    共抓几张(默认 60)
		CaptureScreenSequence();
		wuiBackend.EndFrame(m_WuiContext.Cursor());
		LayerTimingFlush();
	}

	void EditorLayer::CaptureScreenSequence()
	{
		static const char* dir = std::getenv("WLD_SCREEN_CAPTURE_DIR");
		if (!dir || !*dir)
			return;
		static int every = std::getenv("WLD_SCREEN_CAPTURE_EVERY") ? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_EVERY")) : 1;
		static int start = std::getenv("WLD_SCREEN_CAPTURE_START") ? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_START")) : 0;
		static int count = std::getenv("WLD_SCREEN_CAPTURE_COUNT") ? std::atoi(std::getenv("WLD_SCREEN_CAPTURE_COUNT")) : 60;
		static int captured = 0;
		static int frame = 0;
		if (every <= 0)
			every = 1;
		++frame;
		if (frame < start || captured >= count || (frame - start) % every != 0)
			return;
		const std::string path = std::string(dir) + "/screen-" + std::to_string(captured) + ".ppm";
		// CaptureFrame 用 glReadPixels 抓默认帧缓冲(两个后端下主窗口都有 GL 上下文),
		// 抓的是"场景 + WUI 面板"的最终合成结果。
		Renderer::CaptureFrame(path);
		++captured;
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
		// W5-L1:重开/打开成功即用磁盘内容重建外部改动基线(并清掉提示)。
		RebaselineExternalSceneWatch();
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

	// D5c-5:把 3D 轨道相机对齐到当前 2D 视图 —— EditorCamera3D 默认朝向(yaw=pitch=0 → +Z)
	// 与场景/2D 相机(+Z 处朝 -Z)相反,直接切 3D 档会从"背面"看场景,单面几何/蒙皮网格被背面剔除。
	void EditorLayer::AlignEditorCamera3DWithView()
	{
		m_EditorCamera3D.SetYawPitch(180.0f, 0.0f);
		m_EditorCamera3D.FocusOn(glm::vec3(0.0f), 12.0f);
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
		if (m_PreviewRenderer && m_PreviewRenderer->GetColorTexture())
			m_PreviewTextureId = registry.Register(m_PreviewRenderer->GetColorTexture());
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
		{
			m_ScenePaused = !m_ScenePaused;
			// P2 W4:暂停时仍在帧内 Tick 会话(帧末事件派发),但固定步长(计时器/系统)与
			// 可变阶段由 GameApp 的暂停标志冻结,二者必须同步。
			if (Gameplay::GameApp* app = Gameplay::GameApp::TryGet())
				app->SetPaused(m_ScenePaused);
		}
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
			const bool saved = m_Document.SaveTo(std::filesystem::path(path));
			if (saved)
				RebaselineExternalSceneWatch();   // W5-L1:自己写出的内容不该被当成外部改动
			return saved;
		}
		const bool saved = m_Document.SaveTo(m_Document.GetPath());
		if (saved)
			RebaselineExternalSceneWatch();   // W5-L1:同上
		return saved;
	}
	bool EditorLayer::OnKeyPressed(KeyPressedEvent& e)
	{
		if (e.GetRepeatCount() > 0)
			return false;

		bool control = Input::IsKeyPressed(KeyCodes::LeftControl) || Input::IsKeyPressed(KeyCodes::RightControl);
		bool shift = Input::IsKeyPressed(KeyCodes::LeftShift) || Input::IsKeyPressed(KeyCodes::RightShift);
		bool alt = Input::IsKeyPressed(KeyCodes::LeftAlt) || Input::IsKeyPressed(KeyCodes::RightAlt);

		// ---- W9-2:三层快捷键路由(plan B)----
		// GLFW 事件在 UI 帧之后分发,这里读到的 WuiTextFocus 就是"上一帧登记的文本焦点"
		// (各窗口 BeginFrame 清空、渲染期间重新登记)。
		//   ① 文本编辑层(最高):文本控件持有焦点时不触发任何引擎全局命令;
		//      只有 Ctrl 组合键可以下探到第 2 层(脚本编辑器 Ctrl+S/Ctrl+R 等)。
		//   ② 焦点窗口/面板层:EditorShell::FocusedPanel() 的 OnShortcut。
		//   ③ 引擎全局层:未被上面两层消费的按键仍走现有命令表。
		if (Wui::WuiTextFocus::Get().Active())
		{
			if (control)
			{
				if (EditorPanel* panel = m_Shell.FocusedPanel())
				{
					if (panel->OnShortcut(e.GetKeyCode(), control, shift, alt))
						return true;
				}
			}
			return false; // 文本焦点下全局命令一律不触发
		}
		if (EditorPanel* panel = m_Shell.FocusedPanel())
		{
			if (panel->OnShortcut(e.GetKeyCode(), control, shift, alt))
				return true;
		}
		return m_Commands.HandleKey(e.GetKeyCode(), control, shift);
	}

	glm::mat4 EditorLayer::EntityWorldMatrix(Entity entity)
	{
		glm::mat4 world = entity.GetComponent<TransformComponent>().Transform;
		if (entity.HasComponent<WorldTransformComponent>())
			world = entity.GetComponent<WorldTransformComponent>().Matrix;
		return world;
	}

	Entity EditorLayer::GetPreviewCameraEntity() const
	{
		if (!m_ActiveScene)
			return {};
		Entity selected = m_SelectedEntity;
		if (selected.IsValid() && selected.GetScene() == m_ActiveScene.get() &&
			selected.HasComponent<CameraComponent>() && selected.HasComponent<TransformComponent>())
			return selected;
		// 只在**选中相机实体**时才有预览/视锥:未选中相机时编辑器不做任何相机可视化
		// (用户 2026-09-16:"相机预览视口应该只有在选中某个相机时候才展示吧")。
		return {};
	}

	void EditorLayer::RenderCameraPreview()
	{
		if (!m_PreviewRenderer || !m_ActiveScene)
			return;
		Entity cameraEntity = GetPreviewCameraEntity();
		if (!cameraEntity.IsValid())
			return;
		// 与主视口完全同一条提交路径(同一套 SceneRenderer/管线/相机数据),因此
		// "预览分辨率 = 视口分辨率"时,预览图就是该相机看到的画面(验收用的等式)。
		const auto& camera = cameraEntity.GetComponent<CameraComponent>().Camera;
		const glm::mat4 world = EntityWorldMatrix(cameraEntity);
		m_PreviewRenderer->BeginScene(m_ActiveScene.get(), m_RendererOptions);
		m_PreviewRenderer->SubmitScene(camera, world);
		m_PreviewRenderer->EndScene();
	}

	std::string EditorLayer::CameraPreviewLabel() const
	{
		Entity cameraEntity = GetPreviewCameraEntity();
		if (!cameraEntity.IsValid())
			return {};   // 空串 = 当前没有相机预览(面板据此隐藏小窗)
		if (cameraEntity.HasComponent<TagComponent>())
		{
			const std::string& tag = cameraEntity.GetComponent<TagComponent>().Tag;
			if (!tag.empty())
				return tag;
		}
		if (m_ActiveScene && cameraEntity == m_ActiveScene->GetPrimaryCameraEntity())
			return "(primary camera)";
		return "(camera)";
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

	// ---- P2 W5b:脚本热重载(编辑器侧接线)----

	namespace
	{
		const char* ScriptStateText(ScriptInstanceState state)
		{
			switch (state)
			{
				case ScriptInstanceState::Pending: return "Pending";
				case ScriptInstanceState::Creating: return "Creating";
				case ScriptInstanceState::Running: return "Running";
				case ScriptInstanceState::Destroying: return "Destroying";
				case ScriptInstanceState::Stopped: return "Stopped";
				case ScriptInstanceState::Faulted: return "Faulted";
				default: return "?";
			}
		}
	}

	bool EditorLayer::ReloadLuaScriptComponent(LuaScriptComponent& script, Scene* scene, std::string* message)
	{
		auto report = [message](const std::string& text)
		{
			if (message)
				*message = text;
		};
		if (script.ScriptFilePath.empty())
		{
			report("script path is empty");
			return false;
		}
		if (script.State == ScriptInstanceState::Creating || script.State == ScriptInstanceState::Destroying)
		{
			report(std::string("script is ") + ScriptStateText(script.State) + "; retry at a frame boundary");
			return false;
		}
		if (script.IsLoaded && script.State == ScriptInstanceState::Running)
		{
			// 有活动实例:走引擎热重载。失败保留旧版本继续跑(W5a 语义),原因写进组件诊断。
			std::string diagnostics;
			if (ScriptEngine::ReloadScript(script, &diagnostics))
			{
				report(diagnostics.empty() ? "reloaded" : ("reloaded with diagnostics: " + diagnostics));
				return true;
			}
			report(script.ReloadDiagnostic.empty() ? "reload failed" : script.ReloadDiagnostic);
			return false;
		}
		// 没有可重载的活动实例(Faulted / Stopped / Pending):复位 Pending,让 Scene 既有的
		// pending 机制在下一个安全点重新实例化。保留 CachedFields 与 ScriptFilePath(实例状态与
		// 脚本身份),这样"脚本写坏 → 改好 → Reload"能把 Faulted 的实例救回来。
		const ScriptInstanceState before = script.State;
		script.State = ScriptInstanceState::Pending;
		script.IsLoaded = false;
		script.LastError.clear();
		const bool sceneRunning = scene && scene->IsRunning();
		if (before == ScriptInstanceState::Faulted)
			report("reset to Pending for rebuild");
		else if (before == ScriptInstanceState::Pending)
			report(sceneRunning
				? "already pending; the scene will instantiate it on the next update"
				: "queued for rebuild (scene is not playing; the script loads when you press Play)");
		else
			report("reset to Pending for rebuild");
		return true;
	}

	// ---- P2 W8:Scripts 面板的宿主能力 ----

	bool EditorLayer::ScriptsReloadInstance(entt::entity handle, std::string* message)
	{
		auto report = [message](const std::string& text)
		{
			if (message)
				*message = text;
		};
		if (!m_ActiveScene)
		{
			report("no active scene");
			return false;
		}
		if (m_ActiveScene->IsPendingDestroy(handle))
		{
			report("entity is pending destroy");
			return false;
		}
		// Play/Simulate 下活动场景不能用非 const GetRegistry()(断言):与帧边界轮询一样,
		// 走 Entity 的组件指针入口拿到可变组件。
		Entity entity(m_ActiveScene.get(), handle);
		auto* script = static_cast<LuaScriptComponent*>(
			entity.GetComponent(entt::type_id<LuaScriptComponent>().hash()));
		if (!script)
		{
			report("entity has no Lua script component");
			return false;
		}
		const bool ok = ReloadLuaScriptComponent(*script, m_ActiveScene.get(), message);
		WLD_CORE_INFO("[scripts-panel] reload script (handle={0}): {1} ({2})",
			static_cast<uint32_t>(handle), ok ? "applied" : "rejected",
			message ? *message : std::string());
		return ok;
	}

	bool EditorLayer::ScriptsOpenExternal(const std::string& logicalPath, std::string* message)
	{
		auto report = [message](const std::string& text)
		{
			if (message)
				*message = text;
		};
		std::filesystem::path diskPath;
		std::string resolveError;
		if (!ResolveScriptDiskPath(logicalPath, diskPath, &resolveError))
		{
			report(resolveError.empty() ? ("cannot resolve script path: " + logicalPath) : resolveError);
			return false;
		}
		// 先把解析到的绝对路径写进日志/状态:外部程序是否真的起来依赖系统关联,
		// "打开去哪儿"这件事以这里的绝对路径为准(自动化断言它)。
		WLD_CORE_INFO("[scripts-panel] open external: {0} (logical '{1}')", diskPath.string(), logicalPath);
		const HINSTANCE result = ShellExecuteW(nullptr, L"open", diskPath.wstring().c_str(),
			nullptr, nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<INT_PTR>(result) <= 32)
		{
			report("ShellExecuteW failed (code "
				+ std::to_string(static_cast<long long>(reinterpret_cast<INT_PTR>(result)))
				+ ") for " + diskPath.string());
			return false;
		}
		report("opened " + diskPath.string());
		return true;
	}

	bool EditorLayer::ScriptsCreateFromTemplate(std::string& outLogicalPath, std::string* message)
	{
		auto report = [message](const std::string& text)
		{
			if (message)
				*message = text;
		};
		const std::filesystem::path contentRoot = std::filesystem::path(WLD_ASSETPATH);
		const std::filesystem::path templatePath = contentRoot / "scripts" / "templates" / "WorldScript.lua";
		std::error_code templateError;
		if (!std::filesystem::is_regular_file(templatePath, templateError))
		{
			report("script template not found: " + templatePath.string());
			return false;
		}
		for (int index = 1; index <= 10000; ++index)
		{
			const std::string logicalPath = "scripts/script_" + std::to_string(index) + ".lua";
			const std::filesystem::path target = contentRoot / std::filesystem::path(logicalPath);
			std::error_code existsError;
			if (std::filesystem::exists(target, existsError))
				continue;   // 名字已被占用:递增,绝不覆盖
			std::error_code copyError;
			// copy_options::none 在目标存在时失败 —— 这里同时是"不覆盖"的第二道保险。
			std::filesystem::copy_file(templatePath, target, std::filesystem::copy_options::none, copyError);
			if (!copyError)
			{
				outLogicalPath = logicalPath;
				report("created " + logicalPath + " (from templates/WorldScript.lua)");
				WLD_CORE_INFO("[scripts-panel] created script '{0}' from template", logicalPath);
				return true;
			}
			if (copyError == std::make_error_condition(std::errc::file_exists))
				continue;   // 竞态:刚被别处创建 → 名字递增重试
			report("create failed for " + target.string() + ": " + copyError.message());
			return false;
		}
		report("could not find a free scripts/script_<n>.lua name under " + contentRoot.string());
		return false;
	}

	void EditorLayer::PollScriptHotReload(float deltaSeconds)
	{
		// 目标场景:编辑态轮询文档场景;Play/Simulate 轮询正在跑的那个场景。
		Ref<Scene> target = (m_SceneState == SceneState::Edit)
			? m_Document.GetScene()
			: (m_RuntimeScene ? m_RuntimeScene : m_ActiveScene);
		if (!target)
		{
			m_ScriptWatch.Clear();
			m_WatchedScriptPaths.clear();
			m_PendingScriptReloads.clear();
			m_ScriptWatchScene = nullptr;
			return;
		}
		if (m_ScriptWatchScene != target.get())
		{
			// 换场景(进入/退出 Play、开关文档)必须重建基线:旧场景的路径与未决变化不再适用。
			m_ScriptWatch.Clear();
			m_WatchedScriptPaths.clear();
			m_PendingScriptReloads.clear();
			m_ScriptWatchScene = target.get();
		}

		// 只读枚举必须走 const registry:Play/Simulate 下活动场景的非 const GetRegistry()
		// 会触发"活动场景禁止结构写"断言。
		const Scene& scene = *target;
		const entt::registry& registry = scene.GetRegistry();
		std::vector<std::string> paths;
		for (const entt::entity handle : registry.view<LuaScriptComponent>())
		{
			if (scene.IsPendingDestroy(handle))
				continue;
			const auto& script = registry.get<LuaScriptComponent>(handle);
			if (!script.ScriptFilePath.empty())
				paths.push_back(script.ScriptFilePath);
		}
		std::sort(paths.begin(), paths.end());
		paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

		// 监听集合同步:新路径建立基线(首次登记不产生变化),已消失的路径解绑。
		// Watch() 重复调用会重置基线,所以只对未登记的路径调用。
		for (const std::string& path : paths)
			if (std::find(m_WatchedScriptPaths.begin(), m_WatchedScriptPaths.end(), path) == m_WatchedScriptPaths.end())
			{
				m_ScriptWatch.Watch(path);
				m_WatchedScriptPaths.push_back(path);
			}
		for (auto it = m_WatchedScriptPaths.begin(); it != m_WatchedScriptPaths.end();)
		{
			if (std::find(paths.begin(), paths.end(), *it) == paths.end())
			{
				m_ScriptWatch.Unwatch(*it);
				it = m_WatchedScriptPaths.erase(it);
			}
			else
				++it;
		}

		// 已确认的变化先进未决集合:安全点不满足时顺延到下一帧,不丢变化。
		for (const std::string& changed : m_ScriptWatch.Poll(static_cast<double>(deltaSeconds)))
			if (std::find(m_PendingScriptReloads.begin(), m_PendingScriptReloads.end(), changed) == m_PendingScriptReloads.end())
				m_PendingScriptReloads.push_back(changed);
		if (m_PendingScriptReloads.empty())
			return;
		if (!scene.CanApplyScriptReload())
			return;   // 回调内/结构提交点内/停止流程中:下一帧再试

		std::vector<std::string> pending;
		pending.swap(m_PendingScriptReloads);
		for (const std::string& path : pending)
		{
			bool matched = false;
			for (const entt::entity handle : registry.view<LuaScriptComponent>())
			{
				if (scene.IsPendingDestroy(handle))
					continue;
				const auto& probe = registry.get<LuaScriptComponent>(handle);
				if (probe.ScriptFilePath != path)
					continue;
				matched = true;
				// 可变组件引用:运行中的场景不能用非 const GetRegistry()(断言),走 Entity 的
				// 组件指针入口 —— 与属性面板读组件实例是同一条路径。
				Entity entity(target.get(), handle);
				auto* script = static_cast<LuaScriptComponent*>(
					entity.GetComponent(entt::type_id<LuaScriptComponent>().hash()));
				if (!script)
					continue;
				std::string message;
				const bool ok = ReloadLuaScriptComponent(*script, target.get(), &message);
				WLD_CORE_INFO("[hot-reload] {0} script '{1}' (handle={2}): {3}",
					ok ? "applied" : "rejected", path, static_cast<uint32_t>(handle), message);
			}
			if (!matched)
				WLD_CORE_INFO("[hot-reload] changed script '{0}' is no longer used by the current scene; dropped", path);
		}
	}

	std::string EditorLayer::CurrentDocumentLogicalPath() const
	{
		if (!m_Document.HasPath())
			return {};
		const std::filesystem::path documentPath = m_Document.GetPath();
		std::error_code ec;
		std::filesystem::path relative;
		if (documentPath.is_absolute())
		{
			relative = std::filesystem::relative(documentPath, std::filesystem::path(WLD_ASSETPATH), ec);
			if (ec || relative.empty() || relative.is_absolute())
				return {};
		}
		else
		{
			// 相对路径按引擎约定就是"相对内容根"的逻辑路径(与 manifest start_scene 一致)。
			relative = documentPath;
		}
		// 文档在内容根之外(文件对话框里开到别处):只做内存态编辑,不参与外部改动提示。
		if (*relative.begin() == std::filesystem::path(".."))
			return {};
		return relative.generic_string();
	}

	void EditorLayer::RebaselineExternalSceneWatch()
	{
		const std::string logical = CurrentDocumentLogicalPath();
		m_SceneWatch.Clear();
		m_WatchedSceneLogicalPath.clear();
		m_ExternalSceneChanged = false;
		if (logical.empty())
			return;
		m_SceneWatch.Watch(logical);
		m_WatchedSceneLogicalPath = logical;
	}

	void EditorLayer::ReopenExternalScene()
	{
		if (!m_ExternalSceneChanged || !m_Document.HasPath())
			return;
		const std::filesystem::path target = m_Document.GetPath();
		// dirty → 复用未保存确认模态(保存/放弃后才重开);clean → 直接重开。
		// 成功重开会在 DoOpenScene 里重建基线并清掉提示。
		RequestAction([this, target]() { DoOpenScene(target); });
	}

	bool EditorLayer::InstantiateModelFile(const std::string& logicalPath, std::string* message)
	{
		if (m_SceneState != SceneState::Edit || !m_ActiveScene)
		{
			if (message) *message = "模型只能在编辑态实例化(Play/Simulate 下请先退出)";
			return false;
		}
		std::string error;
		const std::size_t created = Gameplay::InstantiateModel(logicalPath, *m_ActiveScene, entt::null, &error);
		if (created == 0)
		{
			if (message) *message = error.empty() ? ("模型里没有可实例化的节点: " + logicalPath) : error;
			return false;
		}
		m_Document.MarkDirty();
		if (message)
			*message = "已实例化 " + std::to_string(created) + " 个实体: " + logicalPath;
		WLD_CORE_INFO("[model] instantiated '{0}': {1} entities", logicalPath, created);
		return true;
	}

	bool EditorLayer::ImportModelFile(const std::string& sourcePath, std::string* message,
		std::string* outLogicalModel, const std::string& destinationLogicalDir)
	{
		if (sourcePath.empty())
		{
			if (message) *message = "导入失败: 空路径";
			return false;
		}
		const std::filesystem::path source = std::filesystem::absolute(sourcePath);
		World::Asset::GltfImportResult imported;
		std::string error;
		if (!World::Asset::ImportFile(source, std::filesystem::path(WLD_ASSETPATH), &imported, &error,
			destinationLogicalDir))
		{
			const std::string failure = "glTF 导入失败: " + (error.empty() ? std::string("未知错误") : error);
			if (message) *message = failure;
			ShowError(failure);   // 菜单/双击都要看得见失败原因
			return false;
		}
		// WModelPath 按约定就是"相对内容根"的逻辑路径(如 models/rock.wmodel);
		// 只有将来它变成绝对路径时才需要再相对化 —— 对相对路径调用 relative() 会得到空串(实测)。
		std::string logicalModel = imported.WModelPath;
		std::replace(logicalModel.begin(), logicalModel.end(), '\\', '/');
		if (std::filesystem::path(logicalModel).is_absolute())
		{
			std::error_code ec;
			const std::filesystem::path relative =
				std::filesystem::relative(logicalModel, std::filesystem::path(WLD_ASSETPATH), ec);
			if (!ec && !relative.empty())
				logicalModel = relative.generic_string();
		}

		if (outLogicalModel)
			*outLogicalModel = logicalModel;
		std::string text = "已导入 " + logicalModel + " (mesh " + std::to_string(imported.MeshCount)
			+ " / submesh " + std::to_string(imported.SubmeshCount)
			+ " / 节点 " + std::to_string(imported.NodeCount)
			+ " / 材质 " + std::to_string(imported.MaterialPaths.size())
			+ ");已打开模型预览(要放进场景在预览里点'放进当前场景')";
		if (message) *message = text;
		// D5c-4b 收尾:导入/重导后清进程级动画模型缓存(AnimationSystem 按 MeshPath 缓存
		// WModelData;不清会让"改了源 → 重导 → 动画还是旧的")。
		AnimationSystem::ClearCache();
		WLD_CORE_INFO("[model] {0}", text);
		return true;
	}

	void EditorLayer::ImportModelDialog()
	{
		std::string path = FileDialogs::OpenFile(
			"glTF Model (*.gltf;*.glb)\0*.gltf;*.glb\0All Files (*.*)\0*.*\0");
		if (path.empty())
			return;
		// D10(用户 2026-09-19):选完源文件后,导入位置由**窗口级居中模态**里的树状选择器选
		// (范围限定在内容根内,不再用原生文件夹对话框 —— 后者可能选到工作区外,
		// 那种位置场景与打包都引用不到)。模态把结果交回同一条导入路径。
		m_Shell.RequestImportDestination(path);
	}

	void EditorLayer::PollAssetHotReload(float deltaSeconds)
	{
		// 开关(环境变量 > 偏好文件):
		//  - WLD_ASSET_HOTRELOAD=0 整体关闭(默认开;自动化脚本用);
		//  - 否则读编辑器偏好"资产热重载"(P4-UX7:以前只能靠环境变量,现在有面板入口)。
		if (const char* switchValue = std::getenv("WLD_ASSET_HOTRELOAD"))
		{
			if (std::string(switchValue) == "0")
				return;
		}
		else if (!Editor::EditorPreferences::Get().Data().AssetHotReload)
		{
			return;
		}

		// 1) 材质/贴图:库内轮询缓存里的 .wmat 与它们引用的贴图(150ms / 500ms)。
		AssetHotReloadReport report;
		MaterialLibrary::Get().PollAssetChanges(static_cast<double>(deltaSeconds), report);
		for (const std::string& path : report.ReloadedMaterials)
			WLD_CORE_INFO("[asset-hot-reload] reloaded material '{0}'", path);
		for (const std::string& path : report.SkippedDirtyMaterials)
			WLD_CORE_INFO("[asset-hot-reload] skipped dirty material '{0}' (unsaved edits kept)", path);
		for (const AssetReloadFailure& failure : report.FailedMaterials)
			WLD_CORE_WARN("[asset-hot-reload] material reload failed '{0}': {1}", failure.Path, failure.Error);
		for (const std::string& path : report.InvalidatedTextures)
			WLD_CORE_INFO("[asset-hot-reload] texture invalidated '{0}'", path);

		// 2) 文档场景(.wd):内容变化只提示 + 一键重开,**不自动替换**(会丢未保存修改,
		//    选择/面板也仍指向旧 Scene 实例)。
		const std::string logical = CurrentDocumentLogicalPath();
		if (logical.empty())
		{
			m_SceneWatch.Clear();
			m_WatchedSceneLogicalPath.clear();
			m_ExternalSceneChanged = false;
			return;
		}
		if (m_WatchedSceneLogicalPath != logical)
		{
			// 换文档:重建基线,不把上一个文档的变化带过来。
			m_SceneWatch.Clear();
			m_SceneWatch.Watch(logical);
			m_WatchedSceneLogicalPath = logical;
			m_ExternalSceneChanged = false;
			return;
		}
		for (const std::string& changed : m_SceneWatch.Poll(static_cast<double>(deltaSeconds)))
		{
			if (!m_ExternalSceneChanged)
				WLD_CORE_INFO("[asset-hot-reload] scene changed '{0}' -> reopen prompt (document kept)", changed);
			m_ExternalSceneChanged = true;
		}
	}

	void EditorLayer::SetSceneState(SceneState state)
	{
		if (m_SceneState == state) return;

		// End the previous mode before replacing any scene references.
		if (m_RuntimeScene)
		{
			// P2 W4:Play 与 Simulate 都由 PlayHost(GameApp 会话)持有运行时生命周期。
			if (m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate)
			{
				m_PlayHost.StopRuntime();
				m_PlayHost.Shutdown();
			}
			if (Gameplay::GameApp* app = Gameplay::GameApp::TryGet())
				app->SetPaused(false);
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
			// P2 W4:Play 与 Simulate 都接入 GameApp 会话(事件/计时器/输入/关卡同一条路径),
			// 两者的差别只在视图口使用哪台相机(Simulate 保持编辑器相机)。
			Gameplay::GameAppDesc desc;
			desc.ProjectId = "worldengine-editor-play";
			desc.FixedStepHz = 60;
			m_PlayHost.Init(desc);
			m_PlayHost.SetRenderer(m_SceneRenderer);
			m_PlayHost.SetScene(m_RuntimeScene, /*startRuntime=*/true);
			if (Gameplay::GameApp* app = Gameplay::GameApp::TryGet())
				app->SetPaused(false);
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


