#include "EditorLayer_Internal.h"

namespace World
{

using namespace EditorLayerDetail;

namespace EditorLayerDetail
{

LayerTiming& LayerTimingState(){
			static LayerTiming timing = [] {
				LayerTiming out;
				const char* env = std::getenv("WLD_FRAME_TIMING");
				out.Enabled = env && env[0] != '\0' && env[0] != '0';
				return out;
			}();
			return timing;
		}


double LayerTimingNowMs(){
			return std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}


void LayerTimingFlush(){
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


		// ---- HOTR-P3-T9:模型重导入的冲突门 ----
		// 壳没有公开的"面板是否打开"查询(形态判定 `PanelStateLabel` 是私有的);公开的
		// `AiDescribeState()` 的 `attach` 段就是它按 `PanelStateLabel` 产出的面板形态清单
		// (attached / floating / hidden),因此这里按它判断 `model:<逻辑路径>` 是否**打开**。
		// 面板里"改了设置还没重导"的脏标记在面板私有状态里,层外面拿不到 ⇒ 打开即跳过
		// (T9 口径的降级分支;跳过只顺延、不丢:面板关闭后自动重试)。
bool IsModelPreviewPanelOpen(const EditorShell& shell, const std::string& assetLogical){
			const std::string panelId = "model:" + assetLogical;
			std::string parseError;   // WuiJson 不接受空 error 指针(内部直接写 *error)
			const std::optional<Wui::JsonValue> state =
				Wui::JsonValue::Parse(shell.AiDescribeState(), &parseError);
			if (!state)
				return false;
			const Wui::JsonValue* attach = state->Find("attach");
			if (attach == nullptr || attach->type != Wui::JsonValue::Type::Array)
				return false;
			for (const Wui::JsonValue& entry : attach->Array)
			{
				const Wui::JsonValue* panel = entry.Find("panel");
				if (panel == nullptr || panel->AsString() != panelId)
					continue;
				const Wui::JsonValue* stateField = entry.Find("state");
				const std::string value = stateField ? stateField->AsString() : std::string();
				return value == "attached" || value == "floating";
			}
			return false;
		}


		// ---- HOTR-P3-T10:资产热重载总闸门 ----
		// 与收敛前 EditorLayer::PollAssetHotReload 开头的判定逐条一致(环境变量 > 偏好文件):
		// WLD_ASSET_HOTRELOAD 设了就以它为准(仅 "0" 关闭);没设就读偏好"资产热重载"。
		// 引擎内建 shader 监听不受这道闸门影响(它有自己的 WLD_SHADER_HOTRELOAD)。
bool AssetHotReloadEnabled(){
			if (const char* switchValue = std::getenv("WLD_ASSET_HOTRELOAD"))
				return std::string(switchValue) != "0";
			return Editor::EditorPreferences::Get().Data().AssetHotReload;
		}


		// ---- Layout-S6:Game 模块加载 = 自动存根生成的前置条件 ----
		//
		// 编辑器的 Lua 存根输入是当前 WorldContext 的 schema 注册表:Game 组件(SampleDataComponent
		// 等)只有在 Game 模块注册后才在表里。而 GameModuleHost::LoadDefault 的兜底路径用
		// WLD_OUTPUT_DIR —— 它由 CMake 定义成**相对**路径("build/x64-<cfg>/"),按进程 CWD 解析:
		//   * CWD = 仓库根(文档口径)→ 命中 build/x64-<cfg>/bin/<cfg>/Game/<cfg>/Game.dll;
		//   * CWD = 别处(双击 Editor.exe / 从构建目录或快捷方式启动)→ 找不到 Game.dll,模块不注册,
		//     于是 ScriptEngine::GenerateLuaStubs() 会把**缺少 Game 组件块**的存根写回入库文件
		//     (内容根 World::Paths::AssetRoot() 默认是绝对路径,写的就是仓库里那一份)。
		//     2026-09-23 实测(cwd=build/x64-Debug):27266 → 26893 字节,少 11 行 SampleDataComponent
		//     块,工作区变脏、World.ScriptWorkflow 的存根漂移门禁失败。
		//
		// 这里补一条**与 CWD 无关**的兜底:把同一个开发期布局锚定到绝对编译期路径(仓库根),
		// 不新增目录约定。WLD_OUTPUT_DIR 将来若改成绝对路径,path 的 / 运算仍取绝对那一侧,
		// 这条兜底继续成立。
		// 仓库根用 WLD_REPO_ROOT(**无**尾分隔符;Layout-S7 新增):WLD_GAME_DIR / WLD_EDITOR_DIR /
		// 编译期宏都写成 "<root>/X/" 带尾分隔符,对它们调 parent_path() 只会去掉那个空文件名
		// (实测 2026-09-23:得到 "<root>/X" 而不是 "<root>",拼出来是
		// "<root>/Game\build/x64-Debug/..." 这种错路径;运行期项目根改走 World::Paths,
		// 它返回**无**尾分隔符的绝对路径)。
bool LoadGameModuleForEditor(WorldContext& context, std::string* error){
			// PROJ-8/T2:当前项目**自己构建**的 Game.dll 优先。项目 C++(<项目根>/src/**)由项目自带
			// build.cmd/CMakeLists 编进 <项目根>/build/x64-<cfg>/bin/<cfg>/Game/<cfg>/Game.dll
			// (与引擎开发布局同形);找不到才退回引擎开发布局(下面两条既有候选)。
			// 加载失败的错误文本一并带回给调用方,日志里能看到"试过哪里、为什么没成"。
			std::string projectNote;
			{
				std::string buildType(WLD_BUILD_TYPE);      // 编译期宏形如 "Debug/"(带尾分隔符)
				while (!buildType.empty() && (buildType.back() == '/' || buildType.back() == '\\'))
					buildType.pop_back();
				const std::filesystem::path projectRoot = World::Paths::ProjectDir();
				if (!projectRoot.empty() && !buildType.empty())
				{
					const std::filesystem::path projectGameDll = projectRoot / "build" / ("x64-" + buildType)
						/ "bin" / buildType / "Game" / buildType / "Game.dll";
					std::error_code existsError;
					if (std::filesystem::is_regular_file(projectGameDll, existsError))
					{
						std::string projectError;
						if (context.Modules().Load(projectGameDll, context, &projectError) == Modules::ModuleManager::Status::Ok)
						{
							WLD_CORE_INFO("[game-module] loaded the project build: {0}", projectGameDll.string());
							return true;
						}
						WLD_CORE_WARN("[game-module] project build rejected: {0} ({1})",
							projectGameDll.string(), projectError);
						projectNote = "project build " + projectGameDll.string() + ": " + projectError + "; ";
					}
					else
					{
						WLD_CORE_INFO("[game-module] no project build at {0}; falling back to the engine build",
							projectGameDll.string());
					}
				}
			}

			std::string primaryError;
			if (Modules::GameModuleHost::LoadDefault(context, &primaryError))
				return true;

			const std::filesystem::path repoRoot = std::filesystem::path(WLD_REPO_ROOT);
			const std::filesystem::path anchored =
				repoRoot / WLD_OUTPUT_DIR / "bin" / WLD_BUILD_TYPE / "Game" / WLD_BUILD_TYPE / "Game.dll";
			std::string anchoredError;
			if (context.Modules().Load(anchored, context, &anchoredError) == Modules::ModuleManager::Status::Ok)
			{
				WLD_CORE_INFO("[game-module] loaded through the workspace-anchored path: {0}", anchored.string());
				return true;
			}

			if (error)
				*error = projectNote + primaryError + "; fallback " + anchored.string() + ": " + anchoredError;
			return false;
		}


		// WLD_PAN_TRACE=1 时把 3D 视口平移的原始鼠标位移与换算后的世界位移打进日志,
		// 供自动化测量"每像素平移多少世界单位"(默认关,零行为变化)。
bool PanTraceEnabled(){
			static const bool enabled = std::getenv("WLD_PAN_TRACE") != nullptr;
			return enabled;
		}


		// CPPT-3:AI `module.status` 的 JSON 文本转义(路径/诊断可能含引号或反斜杠)。
std::string JsonEscape(const std::string& text){
			std::string escaped;
			escaped.reserve(text.size() + 8);
			for (char c : text)
			{
				switch (c)
				{
					case '"': escaped += "\\\""; break;
					case '\\': escaped += "\\\\"; break;
					case '\n': escaped += "\\n"; break;
					case '\r': escaped += "\\r"; break;
					case '\t': escaped += "\\t"; break;
					default: escaped += c; break;
				}
			}
			return escaped;
		}

}

	// PROJ-3/T1:启动器模式在 Application 构造前的一次性准备 —— 口径与理由见 EditorLayer.h
	// 的 LauncherBootScope 注释;这里把"三个动作"落成代码:哨兵项目根 + 临时 CWD + 恢复。
LauncherBootScope::LauncherBootScope(){
		// 1) 项目根 → 哨兵目录(不创建、不读):让 ProjectManifest::Locate 的第 2 步
		//    (World::Paths::ProjectFile)不命中编译期默认值(空的 projects/ 容器)。
		World::Paths::SetProjectDirOverride(std::filesystem::path(WLD_LOCAL_DIR) / "launcher-stub");

		// 2) CWD → Editor.exe 所在目录(双击 exe 时 Explorer 给的 CWD):
		//    第 1 步 cwd/project.we.yaml 与第 2 步"当前项目根"都不命中。
		std::error_code ec;
		m_PreviousWorkingDirectory = std::filesystem::current_path(ec);
		if (ec || m_PreviousWorkingDirectory.empty())
		{
			// 读不到当前 CWD(极罕见):保持原样 —— 退化成"可能仍挂载默认项目",
			// 但不引入新的失败分支(启动器页照常显示)。
			m_PreviousWorkingDirectory.clear();
			return;
		}
		wchar_t exeBuffer[MAX_PATH] = {};
		const DWORD length = GetModuleFileNameW(nullptr, exeBuffer, MAX_PATH);
		if (length == 0 || length >= MAX_PATH)
		{
			m_PreviousWorkingDirectory.clear();   // 没有改过 CWD:不需要恢复
			return;
		}
		std::error_code changeError;
		std::filesystem::current_path(std::filesystem::path(exeBuffer).parent_path(), changeError);
		if (changeError)
			m_PreviousWorkingDirectory.clear();   // 切换失败 = 没有改过 CWD
	}


LauncherBootScope::~LauncherBootScope(){
		if (m_PreviousWorkingDirectory.empty())
			return;
		std::error_code ignored;
		std::filesystem::current_path(m_PreviousWorkingDirectory, ignored);
	}


EditorLayer::EditorLayer(bool projectExplicit, bool launcherMode) : Layer("EditorLayer"), m_Document(Application::Get().GetContext()), m_Shell(*this, launcherMode){
		// PROJ-2/T1:显式项目标记来自宿主(EditorApp 解析 --project / WLD_PROJECT_DIR);
		// 用赋值而不是初始化列表,避免与成员声明顺序无关的重排警告。
		m_ProjectExplicit = projectExplicit;
		// PROJ-3/T1:纯启动器模式(宿主已在 Application 构造前挡住项目挂载,见 LauncherBootScope)。
		m_LauncherMode = launcherMode;
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
		// HOTR-P3-T9:模型自动重导入的冲突门探针 —— 对应面板打开时跳过(只顺延、不覆盖;
		// 面板里的未保存设置看不到,按 T9 的降级分支"打开即跳过")。T10 起经宿主透传。
		m_HotReloadHost.SetModelSkipProbe([this](const std::string& assetLogical)
		{
			return IsModelPreviewPanelOpen(m_Shell, assetLogical);
		});
	}


	// PLUG-T3:析构在 .cpp 定义(unique_ptr<Plugins::PluginManager> 的删除器需要完整类型)。
EditorLayer::~EditorLayer()= default;


	// ---- PLUG-T3:插件系统(引擎根 + 项目根;按 local/plugins.json 跳过被禁用的引擎插件)----
void EditorLayer::LoadDisabledPluginList(){
		m_DisabledPlugins.clear();
		m_PluginDisabledListPath = std::filesystem::path(WLD_LOCAL_DIR) / "plugins.json";
		std::error_code error;
		if (!std::filesystem::is_regular_file(m_PluginDisabledListPath, error))
			return;
		std::ifstream stream(m_PluginDisabledListPath, std::ios::binary);
		if (!stream)
			return;
		const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		std::string parseError;
		const auto parsed = Wui::JsonValue::Parse(text, &parseError);
		if (!parsed)
		{
			WLD_CORE_WARN("[plugin] local/plugins.json parse failed ({0}); treating the list as empty",
				parseError);
			return;
		}
		if (const Wui::JsonValue* disabled = parsed->Find("disabled"))
			for (const Wui::JsonValue& item : disabled->Array)
			{
				const std::string id = item.AsString("");
				if (!id.empty())
					m_DisabledPlugins.insert(id);
			}
		WLD_CORE_INFO("[plugin] local disabled list: {0} plugin(s) [local/plugins.json]",
			m_DisabledPlugins.size());
	}


bool EditorLayer::SaveDisabledPluginList(std::string* error) const{
		try
		{
			Wui::JsonValue root;
			root.type = Wui::JsonValue::Type::Object;
			root.Object.push_back({ "version", Wui::JsonValue::MakeNumber(1) });
			Wui::JsonValue disabled;
			disabled.type = Wui::JsonValue::Type::Array;
			for (const std::string& id : m_DisabledPlugins)   // std::set = 确定性顺序(可复核)
				disabled.Array.push_back(Wui::JsonValue::MakeString(id));
			root.Object.push_back({ "disabled", std::move(disabled) });
			const std::filesystem::path directory = m_PluginDisabledListPath.parent_path();
			std::error_code directoryError;
			if (!directory.empty())
				std::filesystem::create_directories(directory, directoryError);
			std::ofstream stream(m_PluginDisabledListPath, std::ios::binary | std::ios::trunc);
			if (!stream)
			{
				if (error)
					*error = "cannot write " + m_PluginDisabledListPath.generic_string();
				return false;
			}
			stream << root.Dump();
			return true;
		}
		catch (const std::exception& exception)
		{
			if (error)
				*error = exception.what();
			return false;
		}
	}


void EditorLayer::InitPlugins(){
		m_PluginManager = std::make_unique<Plugins::PluginManager>();
		// PLUG-T3b:编辑器扩展面必须在插件加载**之前**接线 —— 插件在 Register 里就要注册
		// 命令 / 面板(否则注册被干净拒绝,插件加载失败)。启动器形态没有编辑器宿主
		// (E2:插件扩展面整体缺席),插件仍可加载但扩展注册会被拒绝并记可读诊断。
		m_PluginManager->SetEditorHost(m_Shell.GetPluginEditorHost());
		LoadDisabledPluginList();
		m_DisabledPluginsAtLoad = m_DisabledPlugins;
		m_PluginLoadErrors.clear();

		const std::filesystem::path engineRoot = std::filesystem::path(WLD_REPO_ROOT) / "plugins";
		const std::filesystem::path projectRoot = World::Paths::ProjectDir() / "plugins";
		// 开发构建的引擎插件产物根(与 Game.dll 查找同一口径的构建树布局):
		// <repo>/<WLD_OUTPUT_DIR>bin/<WLD_BUILD_TYPE>plugins/<WLD_BUILD_TYPE>
		// —— 引擎插件由引擎构建产出,源码树里不写产物;发布布局仍以插件自带 bin/ 为准。
		const std::filesystem::path devEnginePluginBinRoot = std::filesystem::path(WLD_REPO_ROOT)
			/ WLD_OUTPUT_DIR / "bin" / WLD_BUILD_TYPE / "plugins" / WLD_BUILD_TYPE;
		// PLUG-AUTH-1:项目插件的 dev 产物根(项目自己的构建产出):
		// <项目根>/build/x64-<cfg>/bin/<cfg>/plugins/<cfg> —— 与引擎 dev 根同一套拼接口径。
		// 少了这一根,向导新建的项目插件在 dev 形态"发现得到、加载不了"(DLL 找不到)。
		std::string buildConfiguration = WLD_BUILD_TYPE;
		while (!buildConfiguration.empty()
			&& (buildConfiguration.back() == '/' || buildConfiguration.back() == '\\'))
			buildConfiguration.pop_back();
		const std::filesystem::path devProjectPluginBinRoot = World::Paths::ProjectDir()
			/ "build" / ("x64-" + buildConfiguration) / "bin" / buildConfiguration
			/ "plugins" / buildConfiguration;
		std::string discoverError;
		if (!m_PluginManager->Discover(engineRoot, projectRoot,
				{ devEnginePluginBinRoot, devProjectPluginBinRoot }))
			// 只有"已有插件处于 Loaded"才会走到这里(启动路径不会);仍然不阻断编辑器。
			WLD_CORE_ERROR("[plugin] discover failed: {0}", discoverError);

		WorldContext& context = Application::Get().GetContext();
		std::set<std::string> pending;
		for (const Plugins::PluginEntry& entry : m_PluginManager->Entries())
		{
			if (entry.State == Plugins::PluginState::Rejected)
				continue;
			if (m_DisabledPlugins.count(entry.Manifest.Id) > 0)
				continue;   // 本机禁用 = 发现后跳过(不 Register)
			pending.insert(entry.Manifest.Id);
		}
		// 依赖顺序收敛:依赖未就绪的下一轮再试;其它失败逐条记录(条目保持可观测状态)。
		// PluginManager 没有"按子集 LoadAll"的公共 API,这里用"直到没有进展"的循环等价实现 ——
		// 与 LoadAll 同一语义(拓扑序 + 单条失败不影响其余),只是可以跳过被禁用者。
		bool progress = true;
		while (progress && !pending.empty())
		{
			progress = false;
			for (auto iterator = pending.begin(); iterator != pending.end();)
			{
				std::string loadError;
				const Plugins::PluginManager::Status status =
					m_PluginManager->Load(*iterator, context, &loadError);
				if (status == Plugins::PluginManager::Status::Ok)
				{
					iterator = pending.erase(iterator);
					progress = true;
				}
				else if (status == Plugins::PluginManager::Status::DependencyNotLoaded)
				{
					++iterator;   // 依赖还没加载:等下一轮
				}
				else
				{
					const std::string reason = loadError.empty()
						? Plugins::PluginManager::StatusName(status) : loadError;
					m_PluginLoadErrors.emplace(*iterator, reason);
					WLD_CORE_ERROR("[plugin] load failed id={0}: {1}", *iterator, reason);
					iterator = pending.erase(iterator);
					progress = true;
				}
			}
		}
		for (const std::string& id : pending)
		{
			m_PluginLoadErrors.emplace(id, "dependency is not loaded (or dependency cycle)");
			WLD_CORE_ERROR("[plugin] load failed id={0}: dependency is not loaded", id);
		}

		size_t rejected = 0;
		for (const Plugins::PluginEntry& entry : m_PluginManager->Entries())
			if (entry.State == Plugins::PluginState::Rejected)
				++rejected;
		WLD_CORE_INFO("[plugin] summary: discovered={0} loaded={1} rejected={2} disabled={3} "
			"(engine-root={4}, project-root={5})", m_PluginManager->Count(),
			m_PluginManager->LoadedCount(), rejected, m_DisabledPlugins.size(),
			engineRoot.generic_string(), projectRoot.generic_string());
		// PLUG-T3b:插件面板实例 / 布局补建放在加载全部结束之后(中途失败不留半个布局)。
		m_Shell.EnsurePluginPanelsFromRegistry();
	}


void EditorLayer::ShutdownPlugins(){
		if (!m_PluginManager)
			return;
		// PLUG-T3b:先收掉插件面板的窗口/标签/布局记录,再卸载插件(卸载会注销插件回调)。
		m_Shell.ClosePluginPanelsNotInRegistry();
		// 卸载必须在场景/渲染器析构之前(插件可能持有随宿主生命周期释放的资源)。
		m_PluginManager->UnloadAll(Application::Get().GetContext());
		// PLUG-T3b:清空编辑器宿主指针 —— 管理器析构时的兜底回收不再回拨宿主
		// (登记在 UnloadAll 里已经全部注销;管理器必须比 EditorShell 先失效)。
		m_PluginManager->SetEditorHost(nullptr);
		m_PluginManager.reset();
		m_PluginLoadErrors.clear();
	}


bool EditorLayer::IsPluginDisabled(const std::string& id) const{
		return m_DisabledPlugins.count(id) > 0;
	}


bool EditorLayer::PluginRestartPending(const std::string& id) const{
		return IsPluginDisabled(id) != (m_DisabledPluginsAtLoad.count(id) > 0);
	}


std::string EditorLayer::PluginLoadError(const std::string& id) const{
		const auto found = m_PluginLoadErrors.find(id);
		return found == m_PluginLoadErrors.end() ? std::string() : found->second;
	}


bool EditorLayer::SetPluginEnabled(const std::string& id, bool enabled, std::string* message){
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
			return fail(Wui::TrFormat("panel.plugins.notice.not_found", "No plugin with id '{id}'.",
				{ { "id", id } }));
		if (entry->Manifest.Scope != Plugins::PluginScope::Engine)
		{
			// 项目插件随项目加载/禁用由项目清单管;本机开关只管引擎插件(方案 §1)。
			return fail(Wui::Tr("panel.plugins.notice.project_plugin_toggle",
				"Project plugins are loaded with the project — enable/disable them in the project manifest."));
		}
		const bool changed = enabled ? (m_DisabledPlugins.erase(id) > 0)
			: (m_DisabledPlugins.insert(id).second);
		if (changed)
		{
			std::string saveError;
			if (!SaveDisabledPluginList(&saveError))
				return fail(Wui::TrFormat("panel.plugins.notice.save_failed",
					"Could not write local/plugins.json: {reason}", { { "reason", saveError } }));
			WLD_CORE_INFO("[plugin] set_enabled id={0} enabled={1} (local/plugins.json; takes effect "
				"on the next editor start)", id, enabled ? 1 : 0);
		}
		if (message)
			*message = Wui::Tr("panel.plugins.notice.restart_required",
				"Saved to local/plugins.json — restart the editor to apply.");
		return true;
	}

}
