#include "EditorLayer_Internal.h"

namespace World
{

using namespace EditorLayerDetail;


	// ---- PLUG-T6:插件卸载 / 两段式热重载(与 module.reload 的 UX 同款)----------------
bool EditorLayer::UnloadPlugin(const std::string& id, std::string* message){
		const auto fail = [message](const std::string& reason)
		{
			if (message)
				*message = reason;
			return false;
		};
		if (!m_PluginManager)
			return fail(Wui::Tr("panel.plugins.notice.need_project", "Open a project first."));
		std::string error;
		const Plugins::PluginManager::Status status =
			m_PluginManager->Unload(id, Application::Get().GetContext(), &error);
		if (status != Plugins::PluginManager::Status::Ok)
			return fail(error.empty() ? Plugins::PluginManager::StatusName(status) : error);
		// 卸载即消失:插件注册命令/面板的兜底回收已经在 Unload 里完成,这里收掉窗口/标签/布局记录。
		m_Shell.ClosePluginPanelsNotInRegistry();
		WLD_CORE_INFO("[plugin] unloaded on request id={0}", id);
		if (message)
			*message = "plugin unloaded: " + id;
		return true;
	}


bool EditorLayer::ReloadPlugin(const std::string& id, Plugins::PluginReloadResult* result, std::string* message){
		const auto fail = [message](const std::string& reason)
		{
			if (message)
				*message = reason;
			return false;
		};
		if (!m_PluginManager)
			return fail(Wui::Tr("panel.plugins.notice.need_project", "Open a project first."));
		const Plugins::PluginEntry* entry = m_PluginManager->Find(id);
		if (!entry)
			return fail("no plugin with id '" + id + "'");

		Plugins::PluginReloadResult local;
		WorldContext& context = Application::Get().GetContext();
		bool ok = false;
		if (entry->State == Plugins::PluginState::Loaded)
		{
			// 第一段:快照 + 卸载(释放 DLL 文件锁;外部重编后再次调用本命令走第二段)。
			ok = m_PluginManager->UnloadForReload(id, context, m_ActiveScene.get(), &local)
				== Plugins::PluginManager::Status::Ok;
			if (ok)
				m_Shell.ClosePluginPanelsNotInRegistry();
		}
		else
		{
			// 第二段:载入新 DLL + 写回快照;失败时回滚到旧 DLL(旧状态保留)。
			ok = m_PluginManager->LoadForReload(id, context, m_ActiveScene.get(), &local)
				== Plugins::PluginManager::Status::Ok;
			if (ok || local.RolledBack)
				m_Shell.EnsurePluginPanelsFromRegistry();
		}
		if (result)
			*result = local;
		if (message)
			*message = local.Message;
		return ok;
	}


	// HOTR-P3-T8:插件"一键重载"的唯一实现面(面板「重新加载」+ AI `plugin.reload build=1`)。
	// 与 `module.build_reload` 共用同一个 ProjectBuildRunner(全局同一时刻一个构建)。
bool EditorLayer::StartPluginBuildAndReload(const std::string& id, std::string* message){
		const auto reject = [message](const std::string& text)
		{
			if (message)
				*message = text;
			WLD_CORE_WARN("[pluginbuild] rejected: {0}", text);
			return false;
		};
		if (!m_PluginManager)
			return reject(Wui::Tr("panel.plugins.notice.need_project", "Open a project first."));
		const Plugins::PluginEntry* entry = m_PluginManager->Find(id);
		if (!entry)
			return reject(Wui::TrFormat("panel.plugins.notice.not_found", "No plugin with id '{id}'.",
				{ { "id", id } }));
		if (m_ProjectBuildRunner.IsRunning())
			return reject("a project build is already running — wait for it to finish");

		const std::filesystem::path packageDir = m_PluginManager->PluginSourceDir(id);
		const std::string target = m_PluginManager->PluginBuildTarget(id);
		if (packageDir.empty() || target.empty())
			return reject("cannot derive the plugin build target for '" + id + "'");

		std::string configuration(WLD_BUILD_TYPE);   // 编译期宏形如 "Debug/"(带尾分隔符)
		while (!configuration.empty()
			&& (configuration.back() == '/' || configuration.back() == '\\'))
			configuration.pop_back();
		if (configuration.empty())
			return reject("no build configuration (WLD_BUILD_TYPE) for the plugin build");

		// 模板约定:插件目标由**根 CMakeLists** 收集(`plugins/*/CMakeLists.txt` GLOB),
		// 所以项目插件 = 项目根的 CMake 工程,引擎插件 = 引擎根的 CMake 工程。
		std::filesystem::path cmakeSourceDir;
		if (entry->Manifest.Scope == Plugins::PluginScope::Project)
		{
			cmakeSourceDir = World::Paths::ProjectDir();
			if (cmakeSourceDir.empty())
				return reject("no project is open — the project plugin build needs the project root");
		}
		else
		{
			cmakeSourceDir = std::filesystem::path(WLD_REPO_ROOT);
		}
		const std::filesystem::path cmakeBuildDir = cmakeSourceDir / "build" / ("x64-" + configuration);

		// 第一段:仅已加载时快照+卸载(Windows 上编辑器映射着 DLL 时链接器写不进该文件)。
		bool wasLoaded = false;
		if (entry->State == Plugins::PluginState::Loaded)
		{
			wasLoaded = true;
			Plugins::PluginReloadResult unloadResult;
			std::string unloadMessage;
			if (!ReloadPlugin(id, &unloadResult, &unloadMessage))
				return reject(unloadMessage.empty()
					? "cannot unload the plugin before building" : unloadMessage);
		}

		std::string startError;
		if (!m_ProjectBuildRunner.StartCMakeTarget(cmakeSourceDir, cmakeBuildDir, target,
				configuration, WLD_REPO_ROOT, &startError))
		{
			// 罕见(入口前面已检查过):把刚卸载的插件恢复回去,别让一次失败留下空洞。
			if (wasLoaded)
				ReloadPlugin(id);   // 第二段:载入旧 DLL + 写回快照
			return reject(startError.empty() ? "cannot start the plugin build" : startError);
		}
		m_PluginBuildPending = true;
		m_PluginBuildPluginId = id;
		m_PluginManager->BeginPluginBuild(id);
		WLD_CORE_INFO("[pluginbuild] building '{0}' for plugin '{1}' "
			"(source '{2}', build '{3}', config {4})", target, id,
			cmakeSourceDir.generic_string(), cmakeBuildDir.generic_string(), configuration);
		if (message)
			*message = "building " + target + " for plugin " + id;
		return true;
	}


	// PLUG-CLEAN-1:面板「重新加载」按钮的帧边界执行面。
	// 面板在自己的绘制过程中只能登记请求(卸载/装载 DLL 会打断正在进行的面板遍历),
	// 这里在下一帧开头取走:
	//   Build=true(HOTR-P3-T8,面板按钮)→ 快照+卸载 → 后台构建 → 成功后自动加载;
	//   Build=false(旧 RequestReload,保留兼容)→ 与 AI `plugin.reload` 相同的两段式:
	//     loaded → 快照 + 卸载(释放 DLL 文件锁;面板显示 pendingReload);
	//     unloaded + pending → 载入新 DLL + 写回快照(失败回滚,旧状态保留)。
void EditorLayer::ProcessPluginReloadRequests(){
		if (!m_PluginManager)
			return;
		const std::vector<Plugins::PluginManager::PluginReloadRequest> requests =
			m_PluginManager->ConsumeReloadRequestsDetailed();
		for (const Plugins::PluginManager::PluginReloadRequest& request : requests)
		{
			const std::string& id = request.Id;
			if (request.Build)
			{
				std::string message;
				const bool ok = StartPluginBuildAndReload(id, &message);
				WLD_CORE_INFO("[pluginbuild] panel request id={0} ok={1}", id, ok);
				if (message.empty())
				{
					message = ok
						? Wui::TrFormat("panel.plugins.notice.reload_requested",
							"Rebuild & reload requested for '{id}' — it runs at the next frame boundary.",
							{ { "id", id } })
						: Wui::TrFormat("panel.plugins.notice.reload_failed",
							"Plugin reload failed: {id}", { { "id", id } });
				}
				m_Shell.Notify(message);
				continue;
			}
			Plugins::PluginReloadResult result;
			std::string message;
			const bool ok = ReloadPlugin(id, &result, &message);
			WLD_CORE_INFO("[plugin] reload on panel request id={0} ok={1} phase={2} rolledBack={3}",
				id, ok, Plugins::PluginReloadPhaseName(result.ResultPhase), result.RolledBack);
			std::string notice = message;
			if (notice.empty())
			{
				notice = ok
					? Wui::TrFormat("panel.plugins.notice.reload_done", "Plugin reloaded: {id}",
						{ { "id", id } })
					: Wui::TrFormat("panel.plugins.notice.reload_failed", "Plugin reload failed: {id}",
						{ { "id", id } });
			}
			m_Shell.Notify(notice);
		}
	}


void EditorLayer::OnAttach(){
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
		// MAT-UI8:主窗口 WUI 上下文接系统剪贴板 —— 单行文本框(查找/替换、重命名、
		// 属性文本、Combo 过滤等)的 Ctrl+C/X/V。独立窗口在 FloatWindowHost 里各自接线,
		// 两者指向同一份 OS 剪贴板(Windows 走 glfwGet/SetClipboardString)。
		m_WuiContext.GetClipboard = [](std::string& out)
		{
			out = Application::Get().GetWindow().GetClipboardText();
			return !out.empty();
		};
		m_WuiContext.SetClipboard = [](std::string_view text)
		{
			Application::Get().GetWindow().SetClipboardText(std::string(text));
			return true;
		};

		// 2026-09-30 探针钩子:`WLD_VSOPEN_PROBE=<绝对路径>` 启动时走一次真实的
		// "Open in Visual Studio" 路径(配 `WLD_VS_DRYRUN=1` 只打日志、不启动也不投递),
		// 供自动化断言"实例复用 vs 回落启动"。正常编辑不受影响(不设变量 = 零行为变化)。
		// 放在启动器分支之前:两种形态下都可以探到(该路径不依赖已挂载的项目)。
		if (const char* vsProbe = std::getenv("WLD_VSOPEN_PROBE"); vsProbe != nullptr && vsProbe[0] != '\0')
		{
			std::string probeMessage;
			const bool probeOk = OpenInVisualStudio(std::filesystem::path(vsProbe), &probeMessage);
			WLD_CORE_INFO("[vsopen-probe] path={0} ok={1} message={2}", vsProbe, probeOk ? 1 : 0,
				probeMessage);
		}

		// ---- PROJ-3/T1:纯启动器模式(不挂载项目、不做项目级初始化)----
		// 宿主在 Application 构造前已经挡住项目挂载(见 LauncherBootScope);这里:
		//   * 不加载 Game 模块、不生成 Lua 存根、不建 Luau LSP 脚手架、不读项目清单;
		//   * 不新建/打开任何场景(m_ActiveScene 保持空),不创建面板布局与上次窗口恢复;
		//   * 窗口只画一页"项目启动器"(EditorShell::RenderLauncherPage)。
		// 场景渲染目标仍按普通路径创建:`state.dump` 等既有读取路径会读它的尺寸,
		// 而它不接触任何项目内容(渲染器用引擎默认设置初始化)。
		if (m_LauncherMode)
		{
			WLD_CORE_INFO("[launcher] mode=launcher (no project mounted)");
			m_CppModuleStatus.State = CppModuleState::Unloaded;
			m_CppModuleStatus.Ok = false;
			m_CppModuleStatus.Message = "launcher mode: no project mounted";

			m_SceneRenderer = CreateRef<SceneRenderer>();
			m_SceneRenderer->Init();
			m_PreviewRenderer = CreateRef<SceneRenderer>();
			m_PreviewRenderer->Init();
			m_PreviewRenderer->OnResize(480, 270);

			LoadIconTextures();
			RegisterUiTextures();

			// 启动决策(与普通隐式启动同源):
			//   * 偏好 `StartupAutoOpenLastProject` 开着且最近项目第一条有效 ⇒ 第一帧自动重启到它一次
			//     (PROJ-2 语义不变;该偏好的默认值已按 PROJ-3/P1b 改为关);
			//   * 否则停在启动器页(本轮的唯一界面),直到用户选项目、或退出。
			m_ShowProjectLauncher = true;
			if (Editor::EditorPreferences::Get().Data().StartupAutoOpenLastProject)
			{
				const std::vector<Editor::RecentProjectEntry> recent = Editor::ProjectLauncher::LoadRecent();
				if (!recent.empty() && recent.front().Valid)
				{
					m_PendingAutoOpenRecent = true;
					m_PendingAutoOpenProject = std::filesystem::u8path(recent.front().Path);
					WLD_CORE_INFO("[launcher] startup: auto-opening the last project '{0}' once "
						"(preference StartupAutoOpenLastProject is on)", m_PendingAutoOpenProject.u8string());
				}
			}
			return;
		}
		// Layout-S6:Game 模块是 Game 组件 schema 的注册者,也是自动存根生成的输入来源。
		// 加载失败时下面会**停用**自动存根生成(见那儿的原因)。
		std::string moduleError;
		m_GameModuleLoaded = LoadGameModuleForEditor(Application::Get().GetContext(), &moduleError);
		if (!m_GameModuleLoaded)
			WLD_CORE_ERROR("Failed to load Game module: {0}", moduleError);
		// CPPT-3:热重载状态机的初始状态与实况一致(启动加载失败 = unloaded,状态栏显式提示;
		// 该窗口里存根生成被拒绝,见 GenerateLuaStubsAction)。
		m_CppModuleStatus.State = m_GameModuleLoaded ? CppModuleState::Loaded : CppModuleState::Unloaded;
		m_CppModuleStatus.Ok = m_GameModuleLoaded;
		m_CppModuleStatus.Message = m_GameModuleLoaded
			? std::string("Game module loaded") : moduleError;
		// PLUG-T3:插件系统(引擎插件 + 项目插件)—— 发现两个根 + 按本机禁用清单加载。
		// 不依赖 Game 模块成败:插件是独立 ABI;加载失败只记录,不阻断编辑器启动。
		InitPlugins();

		// 开发期资产:编辑器与 Runtime 一致,经 VFS 目录 provider 读内容。
		// 目录 provider 已由 Application::MountProjectContent 统一挂载,此处不再重复。

		// Application initialized Lua before attach; Game registration is now merged.
		// Layout-S6:Game 模块没注册时渲染出来的存根会缺少 Game 组件块,而写出目标是**入库文件**
		// (World::Paths::AssetRoot() 与 CWD 无关,写的就是当前项目里那一份)——宁可保留上一份有效声明,也不要
		// 用不完整的 schema 覆盖它。File ▸ Generate Lua API Stubs 仍可手动强制生成(带告警)。
		if (!m_GameModuleLoaded)
		{
			WLD_CORE_WARN("[Lua] Game module is not registered; skipping the automatic Lua API stub "
				"generation so the committed stub keeps its Game component blocks.");
		}
		else if (!ScriptEngine::GenerateLuaStubs())
			WLD_CORE_ERROR("Automatic Lua API stub generation failed; keeping the last valid declarations.");

		// W8:Luau LSP 脚手架(.vscode/settings.json + .luau-lsp/config.json):create-if-missing,
		// 磁盘已有(用户改过的)配置绝不覆盖。
		{
			// 工作区根 = 项目清单所在目录(清单里的 content_root = assets),与项目里的
			// `.vscode/`、`.luau-lsp/` 一致;没有清单时退回内容根。
			std::filesystem::path scaffoldRoot = World::Paths::AssetRoot();
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

		// ---- PROJ-2/T1:启动决策(显式项目 / 自动打开最近一次 / 项目启动器)----
		//
		// 1) `--project` 或非空 `WLD_PROJECT_DIR` = 显式项目:不显示启动器,顺带记进最近列表
		//    ("编辑器成功挂载某项目后写最近列表"的记录时机);
		// 2) 否则若偏好 StartupAutoOpenLastProject(默认开)且最近列表第一条仍有效 ⇒ 第一帧
		//    自动重启到它一次(RelaunchWithProject;子进程带 --project ⇒ 不再触发,不循环);
		// 3) 否则显示项目启动器(引擎内已无默认项目,隐式启动没有任何项目可落)。
		// 注意 2) 的"不等于当前项目"守卫:隐式启动时哨兵项目根不匹配任何真实条目,
		// 因此最近表里即使有同一路径也会照常自动打开一次,不会来回重启。
		{
			const std::filesystem::path currentRoot = World::Paths::ProjectDir();
			if (m_ProjectExplicit)
			{
				std::string recentError;
				if (Editor::ProjectLauncher::AddRecent(currentRoot, &recentError))
					WLD_CORE_INFO("[project] recent: recorded '{0}'", currentRoot.u8string());
				else
					WLD_CORE_WARN("[project] recent: could not record '{0}': {1}",
						currentRoot.u8string(), recentError);
			}
			else
			{
				const bool autoOpen = Editor::EditorPreferences::Get().Data().StartupAutoOpenLastProject;
				const std::vector<Editor::RecentProjectEntry> recent = autoOpen
					? Editor::ProjectLauncher::LoadRecent() : std::vector<Editor::RecentProjectEntry>();
				if (!recent.empty() && recent.front().Valid)
				{
					const std::filesystem::path root = std::filesystem::u8path(recent.front().Path);
					if (!Editor::ProjectLauncher::SamePath(root, currentRoot))
					{
						m_PendingAutoOpenRecent = true;
						m_PendingAutoOpenProject = root;
						WLD_CORE_INFO("[project] startup: auto-opening the last project '{0}' once "
							"(preference StartupAutoOpenLastProject is on)", root.u8string());
					}
				}
				if (!m_PendingAutoOpenRecent)
				{
					m_ShowProjectLauncher = true;
					WLD_CORE_INFO("[project] startup: no explicit project — showing the project launcher "
						"(current project '{0}')", currentRoot.u8string());
				}
			}
		}
	}


	// 图标是旧式(GL)纹理:窗口/上下文重建后必须重新加载,否则渲染出的图标会错乱。
void EditorLayer::LoadIconTextures(){
		// 图标是**编辑器资源**(Editor/assets/icons),不是内容根里的游戏资产 ——
		// 一律走 EditorResourcePath 拼绝对路径(见 EditorResources.h)。
		// VEC-H6:资产形态 PNG → 引擎单文件容器 `.wtex`(PNG 字节内嵌为 payload,不重编码);
		// `TextureData::LoadTextureData` 的容器分支解码同一份字节 ⇒ 图标外观逐字节不变。
		m_IconPlay = Texture2D::Create(EditorResourcePath("assets/icons/Icon_Play.wtex"));

		m_IconStop = Texture2D::Create(EditorResourcePath("assets/icons/Icon_Stop.wtex"));

		m_IconPause = Texture2D::Create(EditorResourcePath("assets/icons/Icon_Pause.wtex"));
		m_IconContinue = Texture2D::Create(EditorResourcePath("assets/icons/Icon_Continue.wtex"));

		m_IconSimulate = Texture2D::Create(EditorResourcePath("assets/icons/Icon_SimulateStart.wtex"));
		m_IconSimulateStop = Texture2D::Create(EditorResourcePath("assets/icons/Icon_SimulateStop.wtex"));
		m_IconSimulatePause = Texture2D::Create(EditorResourcePath("assets/icons/Icon_SimulatePause.wtex"));
		m_IconSimulateContinue = Texture2D::Create(EditorResourcePath("assets/icons/Icon_SimulateContinue.wtex"));
	}


void EditorLayer::OnDetach(){
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
		// HOTR-P3-T7:在飞的"构建并重载"先收掉 —— 终止整棵构建进程树再 join,
		// 关编辑器不会被一次项目构建(分钟级)阻塞。
		m_ProjectBuildRunner.Shutdown();
		// HOTR-P3-T8:插件"一键重载"的在飞标记同样清掉(快照由 ShutdownPlugins 的
		// UnloadAll 按数据丢失记 ERROR,不会静默留半状态)。
		m_PluginBuildPending = false;
		m_PluginBuildPluginId.clear();

		// HOTR-P3-T10:先停编辑器侧热重载宿主里的全部工作线程并 join(材质 `.slang` 编译线程
		// 只跑 slangc / 读源,纹理重烘线程只跑纯 CPU 烘焙,模型重导入线程只写临时文件;
		// 都不碰 GPU)—— 放在场景/渲染器析构之前,保证 OnDetach 之后不会再有产物进入
		// Install / 落盘。各服务的清基线/清临时文件口径不变。
		m_HotReloadHost.Shutdown();

		SetSceneState(SceneState::Edit);

		// PLUG-T3:插件卸载必须在场景/渲染器析构之前(插件可能引用它们注册的回调/资源)。
		ShutdownPlugins();

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


void EditorLayer::OnUpdate(Timestep ts){
		WLD_PROFILE_FUNCTION();
		LayerTimingScope updateScope(LayerTimingState().Update);
		// D5c-4a:渲染发生在面板绘制里(RenderScene),那里拿不到 Timestep —— 先缓存一帧。
		m_LastDeltaSeconds = ts.GetSeconds();
		ProcessPendingRendererChange();
		// PROJ-2/T1:启动期"自动打开最近项目一次"。放在第一帧(窗口/渲染器已经初始化)执行,
		// 而不是 OnAttach 里直接关进程;RelaunchWithProject 内部仍走未保存确认(启动期无改动)。
		if (m_PendingAutoOpenRecent)
		{
			m_PendingAutoOpenRecent = false;
			const std::filesystem::path root = m_PendingAutoOpenProject;
			m_PendingAutoOpenProject.clear();
			if (!root.empty())
				RelaunchWithProject(root);
		}
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
		// HOTR-P3-T10:编辑器侧热重载统一入口 —— 引擎内建 shader(T3)在帧边界(渲染开始前)
		// 轮询引擎 shader 目录,稳定变化即调 Renderer::ReloadShaders()。刻意放在"没有活动
		// 场景/渲染器"早退**之前**:引擎 shader 与项目无关,启动器/无项目形态同样生效。
		// 资产侧三个 watcher(材质 `.slang` 装配 / `.wtex` 重烘 / glTF 重导入)保持收敛前的
		// 闸门:活动场景 + 渲染器 + 资产热重载开关都满足时才轮询(与下面 PollAssetHotReload
		// 的早退条件同口径);它们的提交(Pump)仍在 PollAssetHotReload 的同一条链上。
		const bool assetHotReloadEnabled =
			m_ActiveScene != nullptr && m_SceneRenderer != nullptr && AssetHotReloadEnabled();
		m_HotReloadHost.Poll(assetHotReloadEnabled, ts.GetSeconds());
		// HOTR-P3-T7:帧边界消费"构建并重载"的后台构建结果(成功 → 加载段;失败 → 保持
		// unloaded)。同样放在"没有活动场景/渲染器"早退之前:构建与场景无关。
		PollCppModuleBuild();
		// HOTR-P3-T8:插件"一键重载"的构建结果(与 module 共用一个后台构建器)。
		PollPluginBuild();
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


void EditorLayer::CaptureFrameIfRequested(){
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


void EditorLayer::RunHierarchyClickCheck(){
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


void EditorLayer::RunPickCheck(){
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



void EditorLayer::OnUiFrame(){
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

		// PLUG-CLEAN-1:插件管理器面板上一帧登记的「重新加载」请求 → 帧边界执行
		// (面板绘制期间不卸载/装载插件 DLL,避免打断正在进行的面板遍历)。
		ProcessPluginReloadRequests();

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


void EditorLayer::CaptureScreenSequence(){
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


void EditorLayer::ExportOperationLog(){
		const std::string path = std::string(WLD_OUTPUT_DIR) + "wui-ops.json";
		std::string error;
		if (m_WuiContext.Ops().Save(path, &error))
			WLD_CORE_INFO("WUI operation log exported to {0}", path);
		else
			WLD_CORE_WARN("Failed to export WUI operation log: {0}", error);
	}


void EditorLayer::OnEvent(Event& event){
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
					if (PanTraceEnabled())
						WLD_CORE_INFO("[dev] 3D pan trace: press button={0} -> mode={1}",
							e.GetMouseButton(), static_cast<int>(m_Viewport3DDragging));
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
					{
						// P4-U13h:鼠标事件是**物理像素**,布局/视口尺寸是**设计单位**(见
						// WuiInputCollector::OnMouseMove 的换算)→ 先把像素位移换算成设计单位,
						// 再按视口高度归一化,平移量与窗口 DPI 缩放无关。
						const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
						// 视口高度(设计单位);首帧布局之前 m_ViewportSize 还是 0,退回窗口高度。
						const float viewportHeight = m_ViewportSize.y > 1.0f
							? m_ViewportSize.y
							: static_cast<float>(Application::Get().GetWindow().GetHeight()) / uiScale;
						// 目标平面处的视锥高度(世界单位)= 2 × 距离 × tan(FOV/2)。
						const float viewportHeightWorld = 2.0f * m_EditorCamera3D.GetDistance()
							* std::tan(glm::radians(m_EditorCamera3D.GetFOV()) * 0.5f);
						const float unitsPerDesignUnit = kPanUnitsPerViewportHeight
							* viewportHeightWorld / std::max(viewportHeight, 1.0f);
						const float dx = (e.GetX() - m_Viewport3DLastMouse.x) / uiScale * unitsPerDesignUnit;
						const float dy = (e.GetY() - m_Viewport3DLastMouse.y) / uiScale * unitsPerDesignUnit;
						if (PanTraceEnabled())
							WLD_CORE_INFO("[dev] 3D pan trace: mouse=({0:.1f},{1:.1f})px uiScale={2:.2f} "
								"viewportH={3:.1f}du worldPerViewport={4:.3f} world=({5:.4f},{6:.4f})",
								e.GetX() - m_Viewport3DLastMouse.x, e.GetY() - m_Viewport3DLastMouse.y,
								uiScale, viewportHeight, viewportHeightWorld, dx, dy);
						m_EditorCamera3D.Pan(dx, dy);
					}
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

bool EditorLayer::OnWindowClose(WindowCloseEvent& e){
		// PROJ-3/T1:启动器模式没有项目/文档 —— 关窗(Alt+F4 / WM_CLOSE)= 退出进程,
		// 不去碰未保存确认/停在空项目上;固定日志 `[launcher] closed; exiting`。
		if (m_LauncherMode)
		{
			RequestLauncherExit();
			e.m_Handled = true;
			return true;
		}
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

void EditorLayer::NewScene(){
		RequestAction([this]() { DoNewScene(); });
	}

void EditorLayer::DoNewScene(){
		SetSceneState(SceneState::Edit);

		m_Document.New();
		UpdateSceneContext(m_Document.GetScene());
	}

void EditorLayer::OpenScene(){
		std::string path = FileDialogs::OpenFile("Scene File (*.wd)\0*.wd\0");
		if (path.empty())
			return;
		OpenScene(std::filesystem::path(path));
	}

void EditorLayer::OpenScene(const std::filesystem::path& path){
		if (path.empty())
			return;
		RequestAction([this, path]() { DoOpenScene(path); });
	}

void EditorLayer::DoOpenScene(const std::filesystem::path& path){
		WLD_CORE_INFO("Opening scene: {0}", path.string());
		if (!m_Document.LoadFromFile(path))
		{
			ShowError(m_Document.GetLastError());
			// HOTR-P2-T5:自动重开失败(文件坏了/正在被写)→ 消费快照 + 重建基线,
			// 不让下一帧拿着同一份未决状态反复重试刷屏。
			if (m_PendingSceneReopen.Pending)
			{
				m_PendingSceneReopen = SceneReopenSnapshot {};
				WLD_CORE_ERROR("[asset-hot-reload] scene auto reopen failed: {0} (the current document is kept)",
					m_Document.GetLastError());
				RebaselineExternalSceneWatch();
			}
			return;
		}
		SetSceneState(SceneState::Edit);
		UpdateSceneContext(m_Document.GetScene());
		// W5-L1:重开/打开成功即用磁盘内容重建外部改动基线(并清掉提示)。
		RebaselineExternalSceneWatch();
		// HOTR-P2-T5:自动重开这一条路径才恢复选择/相机(普通打开/新建/启动场景 = no-op)。
		RestoreSceneReopenSnapshot();
	}

bool EditorLayer::SaveScene(){
		// P4-U13:prefab 会话里 Ctrl+S = 写回那个 .wprefab(而不是场景)。
		if (IsEditingPrefab())
			return SavePrefab();
		if (TrySave())
			return true;
		if (!m_Document.GetLastError().empty())
			ShowError(m_Document.GetLastError());
		return false;
	}

}
