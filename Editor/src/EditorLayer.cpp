#include "EditorLayer.h"
#include "EditorCooker.h"
#include "EditorPreferences.h"
#include "EditorResources.h"
#include "EditorStartup.h"
#include "VisualStudioAutomation.h"
#include "Project/ProjectLauncher.h"
#include "World/Core/Asset/BuiltinImporters.h"
#include "World/Core/Asset/CookPipeline.h"
#include "World/Core/Asset/GltfImporter.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Thread/JobSystem.h"
#include "World/Core/Vfs/DirectoryProvider.h"
#include "World/Core/Vfs/PackageProvider.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/ModelInstance.h"
#include "World/Gameplay/Prefab.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Modules/GameModuleReload.h"
#include "World/Plugins/PluginManager.h"
#include "World/Renderer/RenderSettings.h"
#include "World/Renderer/MaterialLibrary.h"
#include "World/Renderer/AnimationSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Hierarchy.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/HotReload.h"
#include "World/Utils/Paths.h"
#include "World/WUI/WuiRhiBackend.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/WuiScriptedInput.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiJson.h"
#include <filesystem>
#include <fstream>
#include <set>
#include <shellapi.h>
#include <stdexcept>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>
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
		bool LoadGameModuleForEditor(WorldContext& context, std::string* error)
		{
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

		// D7-1a/P4-U13h:3D 视口中键平移的灵敏度系数(单位:个"视口高度对应的世界距离")。
		//
		// 口径:鼠标拖过整个视口高度 ≈ 平移 kPanUnitsPerViewportHeight 个视口高度对应的世界
		// 距离(目标平面处的视锥高度 = 2 × 距离 × tan(FOV/2))。1.0 = 目标平面上的内容与
		// 光标 1:1 跟随(Blender/Unreal 一类编辑器的中键抓取手感)。
		//
		// 旧口径(2026-09-21 前)把像素位移**直接当世界单位**(1.0 世界单位/物理像素,与视口
		// 尺寸、相机距离、FOV 全部无关)。实测默认布局(1280×720 窗口、ui_scale 1.3、三栏
		// 停靠)视口高 281.25 设计单位 = 365.6 物理像素:旧口径拖过整个视口高度平移 366 个
		// 世界单位(场景里的立方体边长才 1~1.6),即用户反馈的"中键拖太灵敏";新口径同样
		// 的整屏拖拽只平移 13.86 个世界单位,每物理像素 0.0379(旧值的 3.79%,≈26× 更慢)。
		//
		// 只作用于平移;右键环绕(1 像素 = 1 度)与滚轮推拉(1 格 = 0.6 世界单位)保持原口径不变。
		constexpr float kPanUnitsPerViewportHeight = 1.0f;

		// WLD_PAN_TRACE=1 时把 3D 视口平移的原始鼠标位移与换算后的世界位移打进日志,
		// 供自动化测量"每像素平移多少世界单位"(默认关,零行为变化)。
		bool PanTraceEnabled()
		{
			static const bool enabled = std::getenv("WLD_PAN_TRACE") != nullptr;
			return enabled;
		}

		// CPPT-3:AI `module.status` 的 JSON 文本转义(路径/诊断可能含引号或反斜杠)。
		std::string JsonEscape(const std::string& text)
		{
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
	LauncherBootScope::LauncherBootScope()
	{
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

	LauncherBootScope::~LauncherBootScope()
	{
		if (m_PreviousWorkingDirectory.empty())
			return;
		std::error_code ignored;
		std::filesystem::current_path(m_PreviousWorkingDirectory, ignored);
	}

	EditorLayer::EditorLayer(bool projectExplicit, bool launcherMode)
		: Layer("EditorLayer"), m_Document(Application::Get().GetContext()), m_Shell(*this, launcherMode)
	{
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
	}

	// PLUG-T3:析构在 .cpp 定义(unique_ptr<Plugins::PluginManager> 的删除器需要完整类型)。
	EditorLayer::~EditorLayer() = default;

	// ---- PLUG-T3:插件系统(引擎根 + 项目根;按 local/plugins.json 跳过被禁用的引擎插件)----
	void EditorLayer::LoadDisabledPluginList()
	{
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

	bool EditorLayer::SaveDisabledPluginList(std::string* error) const
	{
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

	void EditorLayer::InitPlugins()
	{
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

	void EditorLayer::ShutdownPlugins()
	{
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

	bool EditorLayer::IsPluginDisabled(const std::string& id) const
	{
		return m_DisabledPlugins.count(id) > 0;
	}

	bool EditorLayer::PluginRestartPending(const std::string& id) const
	{
		return IsPluginDisabled(id) != (m_DisabledPluginsAtLoad.count(id) > 0);
	}

	std::string EditorLayer::PluginLoadError(const std::string& id) const
	{
		const auto found = m_PluginLoadErrors.find(id);
		return found == m_PluginLoadErrors.end() ? std::string() : found->second;
	}

	bool EditorLayer::SetPluginEnabled(const std::string& id, bool enabled, std::string* message)
	{
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

	// ---- PLUG-T6:插件卸载 / 两段式热重载(与 module.reload 的 UX 同款)----------------
	bool EditorLayer::UnloadPlugin(const std::string& id, std::string* message)
	{
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

	bool EditorLayer::ReloadPlugin(const std::string& id, Plugins::PluginReloadResult* result,
		std::string* message)
	{
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
	bool EditorLayer::StartPluginBuildAndReload(const std::string& id, std::string* message)
	{
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
	void EditorLayer::ProcessPluginReloadRequests()
	{
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
	void EditorLayer::LoadIconTextures()
	{
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
		// HOTR-P3-T7:在飞的"构建并重载"先收掉 —— 终止整棵构建进程树再 join,
		// 关编辑器不会被一次项目构建(分钟级)阻塞。
		m_ProjectBuildRunner.Shutdown();
		// HOTR-P3-T8:插件"一键重载"的在飞标记同样清掉(快照由 ShutdownPlugins 的
		// UnloadAll 按数据丢失记 ERROR,不会静默留半状态)。
		m_PluginBuildPending = false;
		m_PluginBuildPluginId.clear();

		// HOTR-P1-T1:先停材质着色器热重载的编译线程并 join(它只跑 slangc / 读源,不碰 GPU)——
		// 放在场景/渲染器析构之前,保证 OnDetach 之后不会再有产物进入 Install。
		m_ShaderHotReload.Shutdown();
		// HOTR-P1-T3:引擎 shader 监听无工作线程/无 GPU 资源,清基线即可(下次构造重新建立)。
		m_EngineShaderHotReload.Shutdown();
		// HOTR-P2-T5:纹理自动重烘的编码线程先收掉(只跑纯 CPU 烘焙,不碰 GPU/文件写)。
		m_TextureImportWatch.Shutdown();

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

	void EditorLayer::OnUpdate(Timestep ts)
	{
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
		// HOTR-P1-T3:引擎内建 shader 热重载 —— 帧边界(渲染开始前)轮询引擎 shader 目录,
		// 稳定变化即调 Renderer::ReloadShaders()。刻意放在"没有活动场景/渲染器"早退**之前**:
		// 引擎 shader 与项目无关,启动器/无项目形态同样生效。
		m_EngineShaderHotReload.Poll(ts.GetSeconds());
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
	bool EditorLayer::OnWindowClose(WindowCloseEvent& e)
	{
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
	bool EditorLayer::SaveScene()
	{
		// P4-U13:prefab 会话里 Ctrl+S = 写回那个 .wprefab(而不是场景)。
		if (IsEditingPrefab())
			return SavePrefab();
		if (TrySave())
			return true;
		if (!m_Document.GetLastError().empty())
			ShowError(m_Document.GetLastError());
		return false;
	}

	// ---- P4-U13:prefab 文档编辑会话 ----
	//
	// 交互设计(工业引擎同款):双击 .wprefab = 打开编辑(把它当文档),编辑期间顶部有一条
	// 常驻横幅说明"你在改的是资产、不是场景",保存写回资产、返回恢复原来的场景。
	// 文件格式与场景同构(同一个序列化器),因此这里直接复用文档的加载/保存通道。
	void EditorLayer::OpenPrefab(const std::string& logicalPath)
	{
		if (logicalPath.empty())
			return;
		RequestAction([this, logicalPath]() { DoOpenPrefab(logicalPath); });
	}

	// P4-U13c:prefab 资产窗口(看/管理)与编辑会话(OpenPrefab)分开:
	// 窗口只读展示资产;真正的编辑仍然进文档会话(窗口里的 Edit Prefab 调 OpenPrefab)。
	void EditorLayer::OpenPrefabWindow(const std::string& logicalPath)
	{
		if (logicalPath.empty())
			return;
		std::string message;
		// 读不了的资产也要开窗口(状态行写原因),但失败必须留一条可读日志,不能静默。
		if (!m_Shell.OpenPrefabWindowChecked(logicalPath, &message))
			WLD_CORE_WARN("[prefab] window opened for unreadable asset '{0}': {1}", logicalPath, message);
	}

	void EditorLayer::DoOpenPrefab(const std::string& logicalPath)
	{
		const std::filesystem::path absolute = World::Paths::AssetRoot() / logicalPath;
		// 记住"进来之前的场景":返回时原样重开(没有就在返回时给一个空场景)。
		if (!IsEditingPrefab())
		{
			m_SceneBeforePrefab = m_Document.GetPath();
			m_HadSceneBeforePrefab = m_Document.HasPath();
		}
		if (!m_Document.LoadFromFile(absolute))
		{
			ShowError(m_Document.GetLastError());
			return;
		}
		m_PrefabEditLogical = logicalPath;
		SetSceneState(SceneState::Edit);
		UpdateSceneContext(m_Document.GetScene());
		RebaselineExternalSceneWatch();
		WLD_CORE_INFO("[prefab] editing '{0}' (save writes back to the asset)", logicalPath);
	}

	bool EditorLayer::SavePrefab()
	{
		if (!IsEditingPrefab())
			return false;
		const std::filesystem::path absolute = World::Paths::AssetRoot() / m_PrefabEditLogical;
		if (!m_Document.SaveTo(absolute))
		{
			ShowError(m_Document.GetLastError());
			WLD_CORE_WARN("[prefab] save failed: {0}", m_Document.GetLastError());
			return false;
		}
		WLD_CORE_INFO("[prefab] saved '{0}'", m_PrefabEditLogical);
		return true;
	}

	void EditorLayer::ClosePrefab()
	{
		if (!IsEditingPrefab())
			return;
		const std::string logical = m_PrefabEditLogical;
		const std::filesystem::path previous = m_SceneBeforePrefab;
		const bool hadPrevious = m_HadSceneBeforePrefab;
		m_PrefabEditLogical.clear();
		m_SceneBeforePrefab.clear();
		m_HadSceneBeforePrefab = false;
		WLD_CORE_INFO("[prefab] leaving prefab edit session '{0}'", logical);
		if (hadPrevious && !previous.empty())
			DoOpenScene(previous);
		else
			DoNewScene();
	}

	bool EditorLayer::InstantiatePrefabAsset(const std::string& logicalPath, std::string* message)
	{
		if (!m_ActiveScene || logicalPath.empty())
		{
			if (message) *message = "no active scene";
			return false;
		}
		// 与 InstantiateModelFile 同一条只读口径:实例化会写活动场景结构,
		// Play/Simulate 下拒绝(否则 GetRegistry() 的"活动场景禁止结构写"断言会抛出来)。
		if (m_SceneState != SceneState::Edit)
		{
			if (message)
				*message = "预制体只能在编辑态实例化(Play/Simulate 下请先退出)";
			return false;
		}
		const std::filesystem::path absolute = World::Paths::AssetRoot() / logicalPath;
		const Gameplay::PrefabInstanceResult result =
			Gameplay::InstantiateFromFile(absolute, *m_ActiveScene, entt::null);
		if (!result.IsValid())
		{
			if (message) *message = "prefab instantiate failed: " + logicalPath;
			return false;
		}
		// P4-U13b:新入口产生的实例同样登记进场景注册表(层级徽标 / 属性面板实例条 /
		// 右键菜单 / 场景存档都读这一份记录)。路径存**逻辑路径**(与内容浏览器同一约定)。
		m_ActiveScene->AddPrefabInstance(logicalPath, static_cast<entt::entity>(result.Root));
		m_SelectedEntity = result.Root;
		MarkDocumentDirty();
		if (message)
			*message = "已实例化 " + std::to_string(result.EntityCount) + " 个实体: " + logicalPath;
		return true;
	}

	// ---- P4-U13b:prefab 实例(实例条 / 层级徽标共用同一条实现)----
	namespace
	{
		// P4-U13d:子树实体数(写出的 prefab 里会有多少实体)。只走 const 注册表 —— Play/Simulate
		// 下活动场景的非 const GetRegistry() 会触发"结构写"断言;访问集防非法层级死循环。
		uint32_t CountPrefabSubtreeEntities(const Scene& scene, entt::entity root)
		{
			const entt::registry& registry = scene.GetRegistry();
			if (!registry.valid(root))
				return 0;
			std::vector<entt::entity> pending { root };
			std::unordered_set<uint32_t> seen;
			uint32_t count = 0;
			constexpr uint32_t kMaxEntities = 200000;
			while (!pending.empty() && count < kMaxEntities)
			{
				const entt::entity current = pending.back();
				pending.pop_back();
				if (!registry.valid(current) || !seen.insert(static_cast<uint32_t>(current)).second)
					continue;
				++count;
				if (const auto* hierarchy = registry.try_get<HierarchyComponent>(current))
					for (const entt::entity child : hierarchy->Children)
						pending.push_back(child);
			}
			return count;
		}

		// 实体属于哪个实例:自身是实例根,或沿父链找到实例根(深度上限防非法层级死循环)。
		// 只走 const 注册表:Play/Simulate 下活动场景的非 const GetRegistry() 会触发结构写断言,
		// 而实例条在 Play 期间仍然要显示(那时三个动作是禁用的)。
		entt::entity OwningPrefabInstanceRoot(const Scene& scene, entt::entity entity)
		{
			constexpr int kMaxAncestorDepth = 64;
			entt::entity current = entity;
			for (int depth = 0; depth < kMaxAncestorDepth && current != entt::null; ++depth)
			{
				if (scene.FindPrefabInstance(current))
					return current;
				const entt::registry& registry = scene.GetRegistry();
				if (!registry.valid(current))
					break;
				const auto* hierarchy = registry.try_get<HierarchyComponent>(current);
				if (!hierarchy || hierarchy->Parent == entt::null || !registry.valid(hierarchy->Parent))
					break;
				current = hierarchy->Parent;
			}
			return entt::null;
		}
	}

	// ---- P4-U13d:创建预制体(实体子树 → .wprefab 资产)----
	//
	// 一条内核,两个入口:层级面板的"Create Prefab from Selection…"模态与 AI 通道
	// `asset.create_prefab`。口径:
	//  - 只写当前内容根(World::Paths::AssetRoot())内的 .wprefab;缺后缀自动补,
	//    越界/非法字符直接拒绝;
	//  - overwrite=false 且目标已存在 = 失败(绝不静默覆盖,把"覆盖"变成显式决定);
	//  - 成功 = 写盘 + `[prefab] created <逻辑路径> (N entities)` + 内容浏览器选中该资产
	//    + 打开它的 prefab 资产窗口(不进编辑会话,编辑仍要显式点 Edit Prefab)。
	bool EditorLayer::CreatePrefabFromSelection(Entity root, const std::string& logicalPath, bool overwrite,
		std::string* message, PrefabCreateResult* result)
	{
		if (message) message->clear();
		if (m_SceneState != SceneState::Edit)
		{
			if (message) *message = "预制体只能在编辑态创建(Play/Simulate 下请先退出)";
			return false;
		}
		if (!m_ActiveScene || !root.IsValid() || root.GetScene() != m_ActiveScene.get())
		{
			if (message) *message = "没有可导出的实体(先在层级面板里选中一个实体)";
			return false;
		}

		// 逻辑路径:统一分隔符、剥前导斜杠、补 .wprefab(大小写不敏感)。
		std::string logical = logicalPath;
		std::replace(logical.begin(), logical.end(), '\\', '/');
		while (!logical.empty() && logical.front() == '/')
			logical.erase(logical.begin());
		if (logical.empty())
		{
			if (message) *message = "缺少目标路径(如 prefabs/MyCube.wprefab)";
			return false;
		}
		auto lowered = [](std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		};
		constexpr size_t kPrefabSuffixLength = 8;   // ".wprefab"
		const std::string loweredLogical = lowered(logical);
		if (loweredLogical.size() < kPrefabSuffixLength
			|| loweredLogical.compare(loweredLogical.size() - kPrefabSuffixLength, kPrefabSuffixLength, ".wprefab") != 0)
			logical += ".wprefab";

		// 逐段校验:不许空段 / "." / "..",文件名不许含 Windows 非法字符 —— 落点必须留在内容根内。
		for (size_t start = 0; start <= logical.size();)
		{
			const size_t slash = logical.find('/', start);
			const std::string part = logical.substr(start,
				slash == std::string::npos ? std::string::npos : slash - start);
			if (part.empty() || part == "." || part == "..")
			{
				if (message) *message = "非法路径: " + logicalPath + "(不许空目录段 / \"..\")";
				return false;
			}
			if (slash == std::string::npos)
				break;
			start = slash + 1;
		}
		for (const char ch : logical)
		{
			if (ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
			{
				if (message) *message = "非法路径: 不能包含 : * ? \" < > | — " + logicalPath;
				return false;
			}
		}

		const std::filesystem::path absolute = World::Paths::AssetRoot() / std::filesystem::path(logical);
		std::error_code existsError;
		const bool exists = std::filesystem::exists(absolute, existsError);
		if (exists && !overwrite)
		{
			if (message) *message = "目标已存在: " + logical + "(未覆盖;需要覆盖请显式确认)";
			return false;
		}
		if (!absolute.parent_path().empty())
		{
			std::error_code dirError;
			std::filesystem::create_directories(absolute.parent_path(), dirError);
			if (!std::filesystem::is_directory(absolute.parent_path()))
			{
				if (message) *message = "目录创建失败: " + absolute.parent_path().generic_string()
					+ (dirError ? (" (" + dirError.message() + ")") : std::string());
				return false;
			}
		}

		std::string saveError;
		if (!Gameplay::SaveFromScene(*m_ActiveScene, root, absolute, &saveError))
		{
			if (message) *message = saveError.empty() ? ("写盘失败: " + logical) : saveError;
			WLD_CORE_WARN("[prefab] create failed: {0} ({1})", logical, saveError);
			return false;
		}
		const uint32_t entityCount = CountPrefabSubtreeEntities(*m_ActiveScene, static_cast<entt::entity>(root));
		WLD_CORE_INFO("[prefab] created {0} ({1} entities)", logical, entityCount);
		m_WuiContext.RecordOp("prefab", exists ? "overwrite" : "create", logical,
			std::to_string(entityCount));

		// 成功口径:内容浏览器选中该资产 + 打开它的 prefab 资产窗口(编辑仍要显式进会话)。
		m_Shell.SelectContentAsset(logical, "create-prefab");
		std::string openMessage;
		if (!m_Shell.OpenPrefabWindowChecked(logical, &openMessage))
			WLD_CORE_WARN("[prefab] created '{0}' but its asset window could not read it back: {1}",
				logical, openMessage);
		if (result)
		{
			result->LogicalPath = logical;
			result->EntityCount = entityCount;
			result->Overwrote = exists;
		}
		if (message)
			*message = "已创建 " + logical + " (" + std::to_string(entityCount) + " entities)";
		return true;
	}

	// ---- U25-M2:材质工作流:把材质接回场景(编辑器侧唯一写入口)----
	//
	// 与 AI 通道 `scene.set ... Material` / 属性面板用的是**同一个字段**
	// (MeshRendererComponent.MaterialPath,相对内容根):场景存档、渲染器与"撤销本次赋值"
	// 因此天然一致。面板不自己改组件,只通过 PanelHost 调这一条。
	bool EditorLayer::AssignMaterialToEntity(Entity entity, const std::string& logicalPath,
		std::string* message, std::string* outPreviousPath)
	{
		if (message)
			message->clear();
		if (m_SceneState != SceneState::Edit)
		{
			if (message) *message = "材质只能在编辑态赋值(Play/Simulate 下场景只读)";
			return false;
		}
		if (!m_ActiveScene || !entity.IsValid() || entity.GetScene() != m_ActiveScene.get())
		{
			if (message) *message = "没有选中实体(先在层级面板里选中一个实体)";
			return false;
		}
		const entt::entity handle = static_cast<entt::entity>(entity);
		auto& registry = m_ActiveScene->GetRegistry();
		auto* mesh = registry.try_get<MeshRendererComponent>(handle);
		if (!mesh)
		{
			if (message) *message = "选中实体没有 MeshRenderer(材质只能赋给会渲染的实体)";
			return false;
		}
		const std::string normalized = MaterialLibrary::NormalizePath(logicalPath);
		const std::string previous = mesh->MaterialPath;
		const bool changed = previous != normalized;
		mesh->MaterialPath = normalized;
		if (changed)
			MarkDocumentDirty();
		const auto* tag = registry.try_get<TagComponent>(handle);
		const std::string name = tag ? tag->Tag : std::string("(unnamed)");
		WLD_CORE_INFO("[material-ui] assign '{0}' -> entity {1} ('{2}'){3}", normalized,
			static_cast<uint32_t>(handle), name, changed ? "" : " (unchanged)");
		m_WuiContext.RecordOp("material", "assign", normalized, "entity=" + std::to_string(static_cast<uint32_t>(handle)));
		if (outPreviousPath)
			*outPreviousPath = previous;
		if (message)
			*message = (changed ? "已把 " : "已是 ")
				+ (normalized.empty() ? std::string("(none)") : normalized) + " → " + name;
		return true;
	}

	bool EditorLayer::AssignMaterialToSelection(const std::string& logicalPath, Entity* outEntity,
		std::string* outPreviousPath, std::string* message)
	{
		if (outEntity)
			*outEntity = Entity {};
		// 选择模型是**单选**(EditorLayer::m_SelectedEntity):面板的 "Assign to Selection"
		// 交给这里的永远是"第一个(也是唯一一个)选中实体";多选落地后按同一入口逐个调用即可。
		const Entity target = m_SelectedEntity;
		std::string previous;
		if (!AssignMaterialToEntity(target, logicalPath, message, &previous))
			return false;
		if (outEntity)
			*outEntity = target;
		if (outPreviousPath)
			*outPreviousPath = previous;
		return true;
	}

	bool EditorLayer::PrefabInstanceInfo(Entity entity, std::string* sourcePath, size_t* overrideCount,
		Entity* root)
	{
		if (!m_ActiveScene || !entity.IsValid() || entity.GetScene() != m_ActiveScene.get())
			return false;
		const entt::entity rootHandle = OwningPrefabInstanceRoot(
			*m_ActiveScene, static_cast<entt::entity>(entity));
		if (rootHandle == entt::null)
			return false;
		const Gameplay::PrefabInstanceRecord* record = m_ActiveScene->FindPrefabInstance(rootHandle);
		if (!record)
			return false;
		if (sourcePath)
			*sourcePath = record->PrefabPath;
		if (overrideCount)
			*overrideCount = Gameplay::GetOverrideCount(*record);
		if (root)
			*root = Entity(m_ActiveScene.get(), rootHandle);
		return true;
	}

	bool EditorLayer::PrefabInstanceRevert(Entity root, std::string* message)
	{
		// Play/Simulate 下活动场景是播放副本,改它没有意义(与属性面板的只读规则一致)。
		if (m_SceneState != SceneState::Edit)
		{
			if (message) *message = "Play/Simulate 运行中:实例动作只读";
			return false;
		}
		Gameplay::PrefabInstanceRecord* record = m_ActiveScene && root.IsValid()
			? m_ActiveScene->FindPrefabInstance(static_cast<entt::entity>(root)) : nullptr;
		if (!record)
		{
			if (message) *message = "该实体不是 prefab 实例";
			return false;
		}
		if (!Gameplay::RevertInstance(*record, *m_ActiveScene))
		{
			if (message) *message = "回滚失败:来源资产读不到或结构已不匹配";
			return false;
		}
		MarkDocumentDirty();
		if (message) *message = "已回滚到资产";
		return true;
	}

	bool EditorLayer::PrefabInstanceApply(Entity root, std::string* message)
	{
		if (m_SceneState != SceneState::Edit)
		{
			if (message) *message = "Play/Simulate 运行中:实例动作只读";
			return false;
		}
		Gameplay::PrefabInstanceRecord* record = m_ActiveScene && root.IsValid()
			? m_ActiveScene->FindPrefabInstance(static_cast<entt::entity>(root)) : nullptr;
		if (!record)
		{
			if (message) *message = "该实体不是 prefab 实例";
			return false;
		}
		if (record->PrefabPath.empty())
		{
			if (message) *message = "来源资产路径为空,无法写回";
			return false;
		}
		const std::string sourcePath = record->PrefabPath;
		std::string error;
		if (!Gameplay::SaveFromScene(*m_ActiveScene, root, sourcePath, &error))
		{
			if (message) *message = "写回资产失败: " + error;
			return false;
		}
		// 资产已跟上实例 → 覆盖不再是"偏离资产"的记录(与右键菜单同一条口径)。
		Gameplay::ClearOverrides(*record);
		MarkDocumentDirty();
		if (message) *message = "已写回资产: " + sourcePath;
		return true;
	}

	bool EditorLayer::PrefabInstanceUnpack(Entity root, std::string* message)
	{
		if (m_SceneState != SceneState::Edit)
		{
			if (message) *message = "Play/Simulate 运行中:实例动作只读";
			return false;
		}
		const entt::entity handle = root.IsValid()
			? static_cast<entt::entity>(root) : entt::null;
		Gameplay::PrefabInstanceRecord* record = m_ActiveScene && handle != entt::null
			? m_ActiveScene->FindPrefabInstance(handle) : nullptr;
		if (!record)
		{
			if (message) *message = "该实体不是 prefab 实例";
			return false;
		}
		const std::string source = record->PrefabPath;
		if (!Gameplay::UnpackInstance(*record))
		{
			if (message) *message = "断开链接失败";
			return false;
		}
		// 记录本身也要出注册表:之后它就是普通实体(否则实例条会以"空来源"的形态留着)。
		m_ActiveScene->RemovePrefabInstance(handle);
		MarkDocumentDirty();
		if (message) *message = "已断开链接: " + source;
		return true;
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
		// CPPT-3(T5b 硬要求):模块未加载窗口(启动失败 / `unloaded` / `reloading`)**禁止**跑
		// Lua 存根生成 —— Game 组件 schema 不在注册表里,写出来的存根缺 Game 组件块,而目标
		// 是入库文件(Layout-S6 实测:World.ScriptWorkflow 漂移门禁因此变红)。这里从"只警告"
		// 升级为"拒绝执行 + 显式提示";加载或回滚成功后(rolled-back 仍加载着旧模块)恢复可用。
		if (!IsCppModuleLoaded())
		{
			const std::string reason = Wui::Tr("notice.cppmodule.stub_blocked",
				"Lua stub generation is disabled while the C++ module (Game.dll) is not loaded; "
				"reload the module first, then generate the stubs");
			WLD_CORE_WARN("[Lua] {0}", reason);
			m_Shell.Notify(reason);
			return;
		}
		if (!ScriptEngine::GenerateLuaStubs())
			WLD_CORE_ERROR("Lua API stub generation failed; keeping the last valid declarations.");
	}

	// ---- CPPT-3:Game 模块(`Game.dll`)热重载(编辑器入口;plan CPPT-2 §6.1 T5b)----

	const char* EditorLayer::CppModuleStateName() const
	{
		switch (m_CppModuleStatus.State)
		{
			case CppModuleState::Loaded: return "loaded";
			case CppModuleState::Unloaded: return "unloaded";
			case CppModuleState::Reloading: return "reloading";
			case CppModuleState::RolledBack: return "rolled-back";
		}
		return "unloaded";
	}

	bool EditorLayer::IsCppModuleLoaded() const
	{
		// 引擎实况(按模块 id 查当前模块表):重载第一段完成后这里立刻为 false,
		// `reloading` 窗口因此天然落进"未加载"分支(存根门禁、C++ 脚本下拉的未注册提示)。
		return !Modules::GameModuleReload::IsUnloaded(Application::Get().GetContext());
	}

	void EditorLayer::PublishCppModuleResult(const Modules::GameModuleReloadResult& result,
		CppModuleState state, bool ok)
	{
		CppModuleStatus& status = m_CppModuleStatus;
		status.State = state;
		status.Ok = ok;
		status.RolledBack = result.RolledBack;
		status.AbiVersion = result.AbiVersion;
		status.InstancesDrained = result.InstancesDrained;
		status.InstancesRestored = result.InstancesRestored;
		status.ModulePath = result.ModulePath;
		status.Message = result.Message;
		status.Diagnostics = result.Diagnostics;
		++status.Sequence;
		// Layout-S6 的模块标记与实况对齐(自动/手动存根生成的门禁都用它)。
		m_GameModuleLoaded = IsCppModuleLoaded();
	}

	bool EditorLayer::UnloadCppModule(std::string* message)
	{
		WorldContext& context = Application::Get().GetContext();
		if (Modules::GameModuleReload::IsUnloaded(context))
		{
			// 幂等:已经是未加载状态,不报错;把状态对齐成 unloaded(文件可重编)。
			m_CppModuleStatus.State = CppModuleState::Unloaded;
			m_CppModuleStatus.Ok = true;
			m_CppModuleStatus.RolledBack = false;
			m_CppModuleStatus.Message = "Game module is not loaded";
			++m_CppModuleStatus.Sequence;
			m_GameModuleLoaded = false;
			if (message)
				*message = m_CppModuleStatus.Message;
			return true;
		}
		Modules::GameModuleReloadResult result;
		const bool ok = Modules::GameModuleReload::Unload(context, m_ActiveScene.get(), &result);
		// 失败(多数是"不在安全点")时模块仍加载着 —— 不要谎报 unloaded。
		PublishCppModuleResult(result, ok ? CppModuleState::Unloaded : CppModuleState::Loaded, ok);
		WLD_CORE_INFO("[cppmodule] unload ok={0} drained={1} message='{2}'",
			ok ? "true" : "false", result.InstancesDrained, result.Message);
		if (message)
			*message = result.Message;
		return ok;
	}

	bool EditorLayer::ReloadCppModule(std::string* message)
	{
		// HOTR-P3-T7:构建在飞时不允许加载段 —— 否则会把旧/半成品 Game.dll 映射回去,
		// 既可能让链接器写不进去,也让"构建完成后自动加载"失去意义(模块已加载 = 不再加载)。
		if (m_ProjectBuildRunner.IsRunning())
		{
			const std::string error = "a project build is running — wait for it to finish";
			if (message)
				*message = error;
			WLD_CORE_WARN("[cppmodule] reload rejected: {0}", error);
			m_CppModuleStatus.Message = error;
			m_Shell.NotifyCppModuleResult();
			return false;
		}
		WorldContext& context = Application::Get().GetContext();
		Modules::GameModuleReloadResult result;
		if (Modules::GameModuleReload::IsUnloaded(context))
		{
			// 第二段:加载(引擎内含 ABI 等值门 + 失败自动回滚)。
			const bool ok = Modules::GameModuleReload::Load(context, m_ActiveScene.get(), &result);
			const CppModuleState state = ok ? CppModuleState::Loaded
				: (result.RolledBack ? CppModuleState::RolledBack : CppModuleState::Unloaded);
			PublishCppModuleResult(result, state, ok);
			// CPPT-3-FIX1:加载段结束后给一次可见反馈(含迁移诊断的条数与首条文本);
			// 菜单项与 AI `module.reload` 共用这一条,不再由菜单单独 Notify。
			m_Shell.NotifyCppModuleResult();
			WLD_CORE_INFO("[cppmodule] load ok={0} rolledBack={1} abi={2} restored={3} message='{4}'",
				ok ? "true" : "false", result.RolledBack ? "true" : "false", result.AbiVersion,
				result.InstancesRestored, result.Message);
		}
		else
		{
			// 第一段:卸载(解锁 `Game.dll` 供重编);状态停在 reloading,直到再次触发加载新构建。
			const bool ok = Modules::GameModuleReload::Unload(context, m_ActiveScene.get(), &result);
			PublishCppModuleResult(result, ok ? CppModuleState::Reloading : CppModuleState::Loaded, ok);
			m_Shell.NotifyCppModuleResult();
			WLD_CORE_INFO("[cppmodule] reload segment 1 (unload) ok={0} drained={1} message='{2}'",
				ok ? "true" : "false", result.InstancesDrained, result.Message);
		}
		if (message)
			*message = m_CppModuleStatus.Message;
		return m_CppModuleStatus.State == CppModuleState::Reloading || m_CppModuleStatus.Ok;
	}

	// HOTR-P3-T7:File ▸ Build & Reload C++ Module / AI `module.build_reload` 的唯一实现面。
	// 一次调用 = 卸载(先释放 Game.dll 文件锁)→ 后台构建项目自带的 build.cmd → 结果由
	// OnUpdate 的 PollCppModuleBuild 在帧边界消费(成功自动加载 / 失败保持 unloaded)。
	bool EditorLayer::BuildAndReloadCppModule(std::string* message)
	{
		const auto reject = [message](const std::string& text)
		{
			if (message)
				*message = text;
			WLD_CORE_WARN("[cppbuild] rejected: {0}", text);
			return false;
		};

		if (m_ProjectBuildRunner.IsRunning())
			return reject("a project build is already running — wait for it to finish");

		const std::filesystem::path projectRoot = World::Paths::ProjectDir();
		if (projectRoot.empty())
			return reject("no project is open — Build & Reload C++ Module needs a project with its own build.cmd");
		std::error_code scriptError;
		if (!std::filesystem::is_regular_file(projectRoot / "build.cmd", scriptError))
		{
			// 早于卸载做检查:没有构建入口就"干净拒绝",模块保持原状态(不从"已加载"掉到 unloaded)。
			return reject("no build.cmd in " + projectRoot.generic_string()
				+ " — generate the project build entry points first (File ▶ Generate Build Entry Points)");
		}

		WorldContext& context = Application::Get().GetContext();
		const bool wasLoaded = !Modules::GameModuleReload::IsUnloaded(context);
		if (wasLoaded)
		{
			// Windows 上 Editor 映射着 Game.dll 时链接器无法改写该文件 —— 构建前必须先卸载。
			// 不在安全点(Play/Simulate、脚本回调中)时卸载会干净失败,这里原样拒绝、不启动构建。
			std::string unloadMessage;
			if (!UnloadCppModule(&unloadMessage))
				return reject(unloadMessage.empty() ? "cannot unload the Game module" : unloadMessage);
		}

		std::string configuration(WLD_BUILD_TYPE);   // 编译期宏形如 "Debug/"(带尾分隔符)
		while (!configuration.empty()
			&& (configuration.back() == '/' || configuration.back() == '\\'))
			configuration.pop_back();

		std::string startError;
		if (!m_ProjectBuildRunner.Start(projectRoot, WLD_REPO_ROOT, configuration, &startError))
		{
			// 罕见(脚本前面已经校验过):把刚卸载掉的模块恢复回去,别让一次失败留下空洞。
			if (wasLoaded && Modules::GameModuleReload::IsUnloaded(context))
				ReloadCppModule();
			return reject(startError.empty() ? "cannot start the project build" : startError);
		}
		m_CppBuildReloadPending = true;
		WLD_CORE_INFO("[cppbuild] building '{0}' (engine '{1}', config {2}, moduleWasLoaded={3})",
			projectRoot.generic_string(), std::string(WLD_REPO_ROOT), configuration,
			wasLoaded ? "true" : "false");
		if (message)
			*message = "building " + projectRoot.generic_string();
		return true;
	}

	// HOTR-P3-T7:帧边界消费构建结果。成功 → 走两段式的加载段(ABI 等值门 + 失败回滚,与
	// File ▸ Reload C++ Module 的第二段同一入口);失败 → 保持 unloaded,输出尾部进日志与
	// module.status 的 buildOutput 字段(可见、可断言)。
	void EditorLayer::PollCppModuleBuild()
	{
		m_ProjectBuildRunner.Poll();
		if (!m_CppBuildReloadPending || m_ProjectBuildRunner.IsRunning())
			return;
		m_CppBuildReloadPending = false;

		const int exitCode = m_ProjectBuildRunner.ExitCode();
		if (exitCode != 0)
		{
			WLD_CORE_ERROR("[cppbuild] project build failed (exit {0}); Game module stays unloaded", exitCode);
			if (!m_ProjectBuildRunner.OutputTail().empty())
				WLD_CORE_ERROR("[cppbuild] output tail:\n{0}", m_ProjectBuildRunner.OutputTail());
			// 状态语义:模块仍是 unloaded(要重载必须先有可加载的产物);消息里点明构建失败,
			// 输出尾部在日志与 module.status.buildOutput 里。
			m_CppModuleStatus.State = CppModuleState::Unloaded;
			m_CppModuleStatus.Ok = false;
			m_CppModuleStatus.Message = "project build failed (exit code " + std::to_string(exitCode)
				+ "); see the [cppbuild] log lines or module.status buildOutput";
			++m_CppModuleStatus.Sequence;
			m_Shell.NotifyCppModuleResult();
			return;
		}

		WorldContext& context = Application::Get().GetContext();
		if (!Modules::GameModuleReload::IsUnloaded(context))
		{
			// 构建期间有别的入口把旧构建加载回来了(例如 AI `module.reload` 的加载段):
			// 这里不能再走"未加载 → 加载"那一段(它会把已加载的模块又卸载掉)。只提示手动重载。
			WLD_CORE_WARN("[cppbuild] build finished (exit 0) but the Game module is loaded again; "
				"use Build & Reload C++ Module again to swap in the new build");
			m_CppModuleStatus.Message = "project build finished — use Build & Reload C++ Module to load the new build";
			m_Shell.NotifyCppModuleResult();
			return;
		}
		WLD_CORE_INFO("[cppbuild] project build finished (exit 0); loading the new Game.dll");
		ReloadCppModule();
	}

	// HOTR-P3-T8:帧边界消费插件"一键重载"的构建结果(与 module 的构建共用一个 runner,
	// 两者各自有 pending 标记,互不消费对方的结果)。
	void EditorLayer::PollPluginBuild()
	{
		m_ProjectBuildRunner.Poll();
		if (!m_PluginBuildPending || m_ProjectBuildRunner.IsRunning())
			return;
		m_PluginBuildPending = false;
		const std::string id = m_PluginBuildPluginId;
		m_PluginBuildPluginId.clear();

		const int exitCode = m_ProjectBuildRunner.ExitCode();
		const std::string output = m_ProjectBuildRunner.OutputTail();
		if (m_PluginManager)
			m_PluginManager->CompletePluginBuild(id, exitCode, output);
		if (exitCode != 0)
		{
			// 失败语义:插件保持 unloaded(第一段已经卸载;快照仍在内存里),输出尾部进日志与
			// plugin.info 的 buildOutput —— 用户可修源码后再点一次「重新加载」。
			WLD_CORE_ERROR("[pluginbuild] build failed (exit {0}); plugin '{1}' stays unloaded",
				exitCode, id);
			if (!output.empty())
				WLD_CORE_ERROR("[pluginbuild] output tail:\n{0}", output);
			m_Shell.Notify(Wui::TrFormat("panel.plugins.notice.reload_failed",
				"Plugin reload failed: {id}", { { "id", id } }));
			return;
		}

		WLD_CORE_INFO("[pluginbuild] build finished (exit 0); loading the new plugin DLL id={0}", id);
		Plugins::PluginReloadResult result;
		std::string message;
		const bool ok = ReloadPlugin(id, &result, &message);
		WLD_CORE_INFO("[pluginbuild] reload after build id={0} ok={1} phase={2} rolledBack={3}",
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

	std::string EditorLayer::CppModuleStatusJson() const
	{
		const CppModuleStatus& status = m_CppModuleStatus;
		std::ostringstream out;
		out << "{\"command\":\"module.status\""
			<< ",\"state\":\"" << CppModuleStateName() << "\""
			<< ",\"moduleLoaded\":" << (IsCppModuleLoaded() ? "true" : "false")
			<< ",\"ok\":" << (status.Ok ? "true" : "false")
			<< ",\"rolledBack\":" << (status.RolledBack ? "true" : "false")
			<< ",\"abiVersion\":" << status.AbiVersion
			<< ",\"instancesDrained\":" << status.InstancesDrained
			<< ",\"instancesRestored\":" << status.InstancesRestored
			<< ",\"sequence\":" << status.Sequence
			<< ",\"modulePath\":\"" << JsonEscape(status.ModulePath) << "\""
			<< ",\"message\":\"" << JsonEscape(status.Message) << "\""
			<< ",\"diagnostics\":[";
		for (size_t index = 0; index < status.Diagnostics.size(); ++index)
		{
			if (index)
				out << ",";
			out << "\"" << JsonEscape(status.Diagnostics[index]) << "\"";
		}
		// HOTR-P3-T7(append-only):"构建并重载"的进度与最近一次结果。buildRunning 在构建在飞
		// (含"进程已退出但还没被帧边界 Poll 消费")时为真;buildOutput = 最近一次构建的
		// stdout+stderr 尾部(新一次 Start 会清空);没有结果时 buildExitCode = -1。
		out << "],\"buildRunning\":" << (m_ProjectBuildRunner.IsRunning() ? "true" : "false")
			<< ",\"buildExitCode\":" << m_ProjectBuildRunner.ExitCode()
			<< ",\"buildOutput\":\"" << JsonEscape(m_ProjectBuildRunner.OutputTail()) << "\"}";
		return out.str();
	}

	void EditorLayer::CloseAction()
	{
		RequestAction([this]() { World::Application::Get().Close(); });
	}

	// PROJ-3/T1:关闭启动器 = 退出进程(启动器模式没有"当前项目"可停留)。
	// 启动器页的"退出"按钮、Esc 与主窗口关闭事件(Alt+F4 / WM_CLOSE)共用这一条;
	// 日志固定一行,只写一次(两条路径可能在同一帧先后到达)。
	void EditorLayer::RequestLauncherExit()
	{
		if (!m_LauncherExitLogged)
		{
			m_LauncherExitLogged = true;
			WLD_CORE_INFO("[launcher] closed; exiting");
		}
		World::Application::Get().Close();
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

	// ---- PROJ-1/T1:打开另一个项目(重启编辑器进程到该项目根)----
	//
	// 本批不做同进程热切换(见方案 §P4):"打开项目" = 用渲染后端重启那套机制重启自身,
	// 但换掉的不是后端而是**项目根**:
	//   * `--project <root>` 参数(EditorApp 在启动最前面解析,优先级最高)+
	//     `WLD_PROJECT_DIR=<root>` 环境变量(World::Paths 读它);
	//   * 子进程工作目录 = 项目根 —— 项目清单按"CWD → 当前项目根"解析,
	//     CWD 指到项目根后所有面板/内容根/关卡清单都跟着切;
	//   * 丢掉旧的 WLD_START_SCENE(那是上一个项目的场景,不能在新项目里重开)。
	// 有未保存改动时先走既有的未保存确认模态(保存/放弃后才真正重启)。
	void EditorLayer::RelaunchWithProject(const std::filesystem::path& projectRoot)
	{
		if (projectRoot.empty())
			return;
		RequestAction([this, projectRoot]() { DoRelaunchWithProject(projectRoot); });
	}

	void EditorLayer::DoRelaunchWithProject(const std::filesystem::path& projectRoot)
	{
#ifdef WLD_PLATFORM_WINDOWS
		std::error_code absoluteError;
		const std::filesystem::path root = std::filesystem::absolute(projectRoot, absoluteError);
		if (absoluteError || !std::filesystem::is_directory(root))
		{
			const std::string message = "Cannot open the project: the directory does not exist: "
				+ projectRoot.u8string();
			WLD_CORE_ERROR("{0}", message);
			ShowError(message);
			return;
		}

		// PROJ-3/T1:启动器模式拉起编辑器形态时的固定日志(探针断言点)。
		if (m_LauncherMode)
			WLD_CORE_INFO("[launcher] launching editor with --project {0}", root.u8string());

		// PROJ-2/T1:所有"打开项目"的入口(启动器 / File ▸ Open Project / Recent Projects /
		// 向导成功态的"打开项目")都汇聚到这里 —— 真正重启之前把该项目根记进最近列表
		// (子进程 OnAttach 还会再记一次,刷新时间戳)。
		{
			std::string recentError;
			if (Editor::ProjectLauncher::AddRecent(root, &recentError))
				WLD_CORE_INFO("[project] recent: recorded '{0}' (opening)", root.u8string());
			else
				WLD_CORE_WARN("[project] recent: could not record '{0}': {1}", root.u8string(), recentError);
		}

		wchar_t exeBuffer[MAX_PATH] = {};
		if (GetModuleFileNameW(nullptr, exeBuffer, MAX_PATH) == 0)
		{
			ShowError("Cannot open the project: the executable path is unknown.");
			return;
		}
		const std::filesystem::path exePath(exeBuffer);

		// 参数按 UTF-8 → UTF-16 转换(非 ASCII 路径不能按字节直接变宽字符)。
		const std::string arguments = " --project \"" + root.u8string() + "\"";
		std::wstring wideArguments;
		{
			const int wideLength = MultiByteToWideChar(CP_UTF8, 0, arguments.c_str(),
				static_cast<int>(arguments.size()), nullptr, 0);
			wideArguments.resize(static_cast<size_t>(std::max(0, wideLength)));
			if (wideLength > 0)
				MultiByteToWideChar(CP_UTF8, 0, arguments.c_str(), static_cast<int>(arguments.size()),
					wideArguments.data(), wideLength);
		}
		const std::wstring commandLine = L"\"" + exePath.wstring() + L"\"" + wideArguments;
		const std::wstring rootWide = root.wstring();

		// 自定义环境块:沿用当前环境,覆盖 WLD_PROJECT_DIR、丢掉旧的 WLD_START_SCENE。
		std::wstring environmentBlock;
		if (LPWCH current = GetEnvironmentStringsW())
		{
			for (const wchar_t* entry = current; *entry; entry += std::wcslen(entry) + 1)
			{
				if (std::wcsncmp(entry, L"WLD_PROJECT_DIR=", 16) == 0)
					continue;   // 覆盖旧值
				if (std::wcsncmp(entry, L"WLD_START_SCENE=", 16) == 0)
					continue;   // 上一个项目的场景:不带进新项目
				environmentBlock.append(entry);
				environmentBlock.push_back(L'\0');
			}
			FreeEnvironmentStringsW(current);
		}
		environmentBlock.append(L"WLD_PROJECT_DIR=");
		environmentBlock.append(rootWide);
		environmentBlock.push_back(L'\0');
		environmentBlock.push_back(L'\0');

		DWORD lastError = 0;
		STARTUPINFOW startup {};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process {};
		std::wstring writableCommandLine = commandLine;   // CreateProcess 可能修改该缓冲
		const BOOL spawned = CreateProcessW(exePath.wstring().c_str(), writableCommandLine.data(),
			nullptr, nullptr, FALSE, 0, environmentBlock.data(), rootWide.c_str(), &startup, &process);
		bool launched = spawned != FALSE;
		if (launched)
		{
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
		}
		else
		{
			lastError = GetLastError();
			// CreateProcess 可能被作业对象/权限策略拦下;退一步交给 Shell 启动
			// (ShellExecuteW 的 lpDirectory 同样把工作目录设成项目根,参数里带着 --project)。
			const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open", exePath.wstring().c_str(),
				wideArguments.empty() ? nullptr : wideArguments.c_str(), rootWide.c_str(), SW_SHOWNORMAL);
			if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
			{
				const std::string message = "Opening the project requires an editor restart, but relaunching "
					"the editor failed (CreateProcess error " + std::to_string(lastError)
					+ ", ShellExecute error "
					+ std::to_string(static_cast<int>(reinterpret_cast<INT_PTR>(shellResult))) + ")."
					" The project was created; start the editor with --project \"" + root.u8string()
					+ "\" manually.";
				WLD_CORE_ERROR("{0}", message);
				ShowError(message);
				return;
			}
			launched = true;
			WLD_CORE_INFO("Editor relaunched via ShellExecute (CreateProcess error {0})", lastError);
		}

		WLD_CORE_INFO("Editor restarted into project '{0}'", root.u8string());
		Application::Get().Close();
#else
		const std::string message = "Opening another project requires an editor restart on this platform ("
			+ projectRoot.u8string() + ").";
		WLD_CORE_WARN("{0}", message);
		ShowError(message);
#endif
	}

	// ---- PROJ-2/T1:项目启动器(状态) + 一键启动项目(独立 Runtime 进程)----
	void EditorLayer::DismissProjectLauncher()
	{
		// PROJ-3/T1:启动器模式没有"当前项目"可停留 —— 启动器页上的"退出"走
		// RequestLauncherExit();这里只服务于普通编辑器形态的模态启动器(PROJ-2 语义)。
		if (m_LauncherMode)
			return;
		if (!m_ShowProjectLauncher)
			return;
		m_ShowProjectLauncher = false;
		WLD_CORE_INFO("[project] launcher closed; staying in the current project '{0}'",
			World::Paths::ProjectDir().u8string());
	}

	void EditorLayer::LaunchProjectRuntime(const std::filesystem::path& projectRoot)
	{
#ifdef WLD_PLATFORM_WINDOWS
		std::error_code absoluteError;
		const std::filesystem::path root = std::filesystem::absolute(projectRoot, absoluteError)
			.lexically_normal();
		if (absoluteError || !std::filesystem::is_directory(root))
		{
			ShowError("Cannot launch the project: the directory does not exist: " + projectRoot.u8string());
			return;
		}

		// 开发布局定位(与 EditorCooker 拷贝 Runtime 的口径一致):
		//   <WLD_REPO_ROOT>/<WLD_OUTPUT_DIR>Runtime/<WLD_BUILD_TYPE>/Runtime.exe
		// 两个宏本身带尾分隔符,拼出来与 cooker 的字符串拼接逐字节等价;lexically_normal()
		// 只用于日志/通知里的可读路径(与进程 CWD 无关)。
		const std::filesystem::path runtimeDirectory =
			std::filesystem::path(WLD_REPO_ROOT) / WLD_OUTPUT_DIR / "Runtime" / WLD_BUILD_TYPE;
		const std::filesystem::path runtimeExe = runtimeDirectory / "Runtime.exe";
		const std::filesystem::path runtimeDll = runtimeDirectory / "WorldRuntime.dll";
		std::error_code exeError;
		std::error_code dllError;
		const bool runtimePresent = std::filesystem::is_regular_file(runtimeExe, exeError) &&
			std::filesystem::is_regular_file(runtimeDll, dllError);

		const std::string exeText = runtimeExe.lexically_normal().u8string();
		const std::string projectText = root.u8string();
		if (!runtimePresent)
		{
			// 缺失 ⇒ 通知里给一条可复制的构建命令(目录/配置取自编译期宏,不是写死 Debug)。
			std::string buildDirectory = std::string(WLD_OUTPUT_DIR);
			while (!buildDirectory.empty() && (buildDirectory.back() == '/' || buildDirectory.back() == '\\'))
				buildDirectory.pop_back();
			std::string configuration = std::string(WLD_BUILD_TYPE);
			while (!configuration.empty() && (configuration.back() == '/' || configuration.back() == '\\'))
				configuration.pop_back();
			const std::string command = "cmake --build " + buildDirectory + " --config " + configuration
				+ " --target Runtime";
			WLD_CORE_ERROR("[project] launch runtime: exe={0} project={1} mode=missing", exeText, projectText);
			WLD_CORE_ERROR("[project] launch runtime: Runtime build is missing — {0}", command);
			m_Shell.Notify(Wui::TrFormat("notice.project.runtime_missing",
				"Runtime is not built yet ({exe}). Build it first: {command}",
				{ { "exe", exeText }, { "command", command } }));
			m_WuiContext.RecordOp("project", "run-failed", Editor::ProjectLauncher::DisplayName(root), projectText);
			return;
		}

		// 参数按 UTF-8 → UTF-16 转换(非 ASCII 项目路径不能按字节直接变宽字符);
		// 工作目录 = 项目根(清单/内容根按 CWD → 项目根解析)。
		const std::string arguments = " --project \"" + projectText + "\"";
		std::wstring wideArguments;
		{
			const int wideLength = MultiByteToWideChar(CP_UTF8, 0, arguments.c_str(),
				static_cast<int>(arguments.size()), nullptr, 0);
			wideArguments.resize(static_cast<size_t>(std::max(0, wideLength)));
			if (wideLength > 0)
				MultiByteToWideChar(CP_UTF8, 0, arguments.c_str(), static_cast<int>(arguments.size()),
					wideArguments.data(), wideLength);
		}
		const std::wstring commandLine = L"\"" + runtimeExe.wstring() + L"\"" + wideArguments;
		const std::wstring workingDirectory = root.wstring();

		DWORD lastError = 0;
		std::string mode;
		STARTUPINFOW startup {};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process {};
		std::wstring writableCommandLine = commandLine;   // CreateProcess 可能修改该缓冲
		const BOOL spawned = CreateProcessW(runtimeExe.wstring().c_str(), writableCommandLine.data(),
			nullptr, nullptr, FALSE, 0, nullptr, workingDirectory.c_str(), &startup, &process);
		if (spawned)
		{
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			mode = "launch";
		}
		else
		{
			lastError = GetLastError();
			// CreateProcess 可能被作业对象/权限策略拦下;退一步交给 Shell 启动
			// (ShellExecuteW 的 lpDirectory 同样把工作目录设成项目根,参数里带着 --project)。
			const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open", runtimeExe.wstring().c_str(),
				wideArguments.empty() ? nullptr : wideArguments.c_str(), workingDirectory.c_str(),
				SW_SHOWNORMAL);
			if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
			{
				const std::string message = "Launching the project runtime failed (CreateProcess error "
					+ std::to_string(static_cast<unsigned long>(lastError)) + ", ShellExecute error "
					+ std::to_string(static_cast<int>(reinterpret_cast<INT_PTR>(shellResult))) + "): "
					+ exeText;
				WLD_CORE_ERROR("[project] launch runtime: exe={0} project={1} launch-failed", exeText, projectText);
				WLD_CORE_ERROR("{0}", message);
				ShowError(message);
				m_WuiContext.RecordOp("project", "run-failed", Editor::ProjectLauncher::DisplayName(root), projectText);
				return;
			}
			mode = "shell";
		}

		WLD_CORE_INFO("[project] launch runtime: exe={0} project={1} mode={2}", exeText, projectText, mode);
		m_WuiContext.RecordOp("project", "run", Editor::ProjectLauncher::DisplayName(root), projectText);
		m_Shell.Notify(Wui::TrFormat("notice.project.runtime_launched",
			"Started the project runtime: {path}", { { "path", projectText } }));
#else
		// 非 Windows 平台本批不提供独立启动(与 Editor 其它重启路径同口径)。
		WLD_CORE_WARN("Launching the project runtime is not supported on this platform ({0}).",
			projectRoot.u8string());
		ShowError("Launching the project runtime is not supported on this platform.");
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
		// PROJ-3/T1:启动器模式没有场景/面板 —— 全局命令表(新建/打开/保存场景、Play、Gizmo…)
		// 一律不触发;启动器页的键盘交互(Esc = 退出)由 WUI 自己的输入路径消费。
		if (m_LauncherMode)
			return false;
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

	bool EditorLayer::ReloadLuauScriptComponent(LuauScriptComponent& script, Scene* scene, std::string* message)
	{
		auto report = [message](const std::string& text)
		{
			if (message)
				*message = text;
		};
		if (script.ScriptPath.empty())
		{
			report("script path is empty");
			return false;
		}
		if (script.Runtime.State == ScriptInstanceState::Creating
			|| script.Runtime.State == ScriptInstanceState::Destroying)
		{
			report(std::string("script is ") + ScriptStateText(script.Runtime.State)
				+ "; retry at a frame boundary");
			return false;
		}
		// 2026-09-26 重写:旧的双轨加载标记已删除 —— "有活动实例"的等价口径就是 State==Running。
		if (script.Runtime.State == ScriptInstanceState::Running)
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
		// pending 机制在下一个安全点重新实例化。保留 Properties 与 ScriptPath(属性值与脚本
		// 身份),这样"脚本写坏 → 改好 → Reload"能把 Faulted 的实例救回来;运行态(含旧错误)
		// 清干净,重新实例化时由引擎重填。
		const ScriptInstanceState before = script.Runtime.State;
		script.Runtime.State = ScriptInstanceState::Pending;
		script.Runtime.LastError.clear();
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
		auto* script = static_cast<LuauScriptComponent*>(
			entity.GetComponent(entt::type_id<LuauScriptComponent>().hash()));
		if (!script)
		{
			report("entity has no Lua script component");
			return false;
		}
		const bool ok = ReloadLuauScriptComponent(*script, m_ActiveScene.get(), message);
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

	namespace
	{
		// CPPT-7/PROJ-8:vswhere → devenv.exe。会话内只解析一次(VS 安装不会在编辑会话中途变化),
		// 结果空 = 没找到(调用方给可读提示,不静默走系统文件关联)。
		const std::filesystem::path& VisualStudioDevenvPath()
		{
			static bool resolved = false;
			static std::filesystem::path cached;
			if (resolved)
				return cached;
			resolved = true;

			const char* programFiles = std::getenv("ProgramFiles(x86)");
			if (programFiles == nullptr || programFiles[0] == '\0')
				programFiles = std::getenv("ProgramFiles");
			if (programFiles == nullptr || programFiles[0] == '\0')
			{
				WLD_CORE_WARN("[vsopen] neither ProgramFiles(x86) nor ProgramFiles is set; cannot locate vswhere");
				return cached;
			}
			const std::filesystem::path vswhere = std::filesystem::path(programFiles)
				/ "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
			std::error_code fileError;
			if (!std::filesystem::is_regular_file(vswhere, fileError))
			{
				WLD_CORE_WARN("[vsopen] vswhere.exe not found: {0}", vswhere.string());
				return cached;
			}

			SECURITY_ATTRIBUTES attributes {};
			attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
			attributes.bInheritHandle = TRUE;
			HANDLE readPipe = nullptr;
			HANDLE writePipe = nullptr;
			if (!CreatePipe(&readPipe, &writePipe, &attributes, 0))
			{
				WLD_CORE_WARN("[vsopen] CreatePipe failed ({0}); cannot run vswhere", GetLastError());
				return cached;
			}
			SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

			STARTUPINFOW startup {};
			startup.cb = sizeof(STARTUPINFOW);
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdOutput = writePipe;
			startup.hStdError = writePipe;
			PROCESS_INFORMATION process {};
			std::wstring commandLine = L"\"" + vswhere.wstring() + L"\" -latest -property productPath";
			std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
			commandBuffer.push_back(L'\0');
			const BOOL spawned = CreateProcessW(vswhere.wstring().c_str(), commandBuffer.data(),
				nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
			CloseHandle(writePipe);

			std::string output;
			if (spawned)
			{
				// 先读完输出再等进程:输出量很小(<1KB),这样不会因为写满管道而死锁。
				char buffer[512];
				DWORD read = 0;
				while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
					output.append(buffer, read);
				WaitForSingleObject(process.hProcess, 20000);
				CloseHandle(process.hThread);
				CloseHandle(process.hProcess);
			}
			else
			{
				WLD_CORE_WARN("[vsopen] vswhere failed to start ({0})", GetLastError());
			}
			CloseHandle(readPipe);

			while (!output.empty() && (output.back() == '\n' || output.back() == '\r'
				|| output.back() == ' ' || output.back() == '\t'))
				output.pop_back();
			const std::size_t first = output.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
			{
				WLD_CORE_WARN("[vsopen] vswhere reported no productPath (is a Visual Studio C++ installation present?)");
				return cached;
			}
			output.erase(0, first);
			// vswhere 的 productPath 是安装布局里的 ASCII 路径;按窄字符构造后仍要落盘校验。
			const std::filesystem::path devenv(output);
			if (std::filesystem::is_regular_file(devenv, fileError))
				cached = devenv;
			else
				WLD_CORE_WARN("[vsopen] vswhere reported a productPath that is not a file: {0}", output);
			return cached;
		}
	}

	bool EditorLayer::OpenInVisualStudio(const std::filesystem::path& absPath, std::string* message)
	{
		auto report = [message](const std::string& text)
		{
			if (message)
				*message = text;
		};
		std::error_code fileError;
		if (absPath.empty() || !std::filesystem::is_regular_file(absPath, fileError))
		{
			WLD_CORE_WARN("[vsopen] file={0} devenv= mode=none (file not found)", absPath.string());
			report("file not found: " + absPath.string());
			return false;
		}

		// ---- 2026-09-30:优先"跳转已经在跑的 VS 实例" ----
		// 只要有一个 VS 实例打开着该文件所属的根(引擎根 / 项目根),就在**那个实例**里
		// 打开文件并激活窗口,不再起新实例。归属判定用"根最长前缀"——Open-Folder(CMake)
		// 模式下 Solution.FindProjectItem 返回 null(2026-09-30 实测),不能拿它判归属。
		// 枚举与打开都限时(默认 3s),任何失败/超时都回落到下面的"启动新实例"三模式。
		{
			const std::vector<Editor::RunningVisualStudio> instances = Editor::EnumerateRunningVisualStudio();
			if (!instances.empty())
			{
				std::string roots;
				for (const Editor::RunningVisualStudio& instance : instances)
				{
					if (!roots.empty())
						roots += ", ";
					roots += std::filesystem::path(instance.Root).u8string();
				}
				WLD_CORE_INFO("[vsopen] running Visual Studio instances={0} roots=[{1}]",
					instances.size(), roots);
			}
			Editor::RunningVisualStudio matched;
			if (Editor::FindRunningVisualStudioFor(instances, absPath, &matched))
			{
				const std::string rootText = std::filesystem::path(matched.Root).u8string();
				const char* dryRunEnv = std::getenv("WLD_VS_DRYRUN");
				const bool dryRun = dryRunEnv != nullptr && dryRunEnv[0] != '\0'
					&& std::strcmp(dryRunEnv, "0") != 0;
				if (dryRun)
				{
					WLD_CORE_INFO("[vsopen] file={0} devenv= mode=dte-match instance-root={1}",
						absPath.generic_string(), rootText);
					report("dry-run: would open " + absPath.string()
						+ " in the running Visual Studio instance at " + rootText);
					return true;
				}
				std::string openError;
				if (Editor::OpenFileInRunningVisualStudio(matched, absPath, &openError))
				{
					WLD_CORE_INFO("[vsopen] file={0} devenv= mode=dte instance-root={1} pid={2}",
						absPath.generic_string(), rootText, matched.Pid);
					report("opened in the running Visual Studio instance: " + absPath.string());
					return true;
				}
				WLD_CORE_WARN("[vsopen] running instance open failed ({0}); falling back to launch",
					openError);
			}
		}

		// 打开策略:项目 CMake → 引擎解决方案 → 单文件(按文件归属判定,见头文件注释)。
		const std::filesystem::path devenv = VisualStudioDevenvPath();
		std::filesystem::path target = absPath;
		std::string mode = "file";
		const std::filesystem::path projectRoot = World::Paths::ProjectDir();
		const bool hasManifest = !projectRoot.empty()
			&& std::filesystem::is_regular_file(projectRoot / "project.we.yaml", fileError);
		const std::string projectRelative = projectRoot.empty()
			? std::string() : absPath.lexically_relative(projectRoot).generic_string();
		const bool insideProject = hasManifest && !projectRelative.empty()
			&& projectRelative.compare(0, 2, "..") != 0;
		const std::filesystem::path projectCMake = projectRoot / "CMakeLists.txt";
		if (insideProject && std::filesystem::is_regular_file(projectCMake, fileError))
		{
			// VS 的 CMake 模式:打开 CMakeLists.txt,VS 自己接管 configure/IntelliSense。
			target = projectCMake;
			mode = "cmake";
		}
		else
		{
			const std::filesystem::path engineRoot = std::filesystem::path(WLD_REPO_ROOT) / "Engine";
			const std::string engineRelative = absPath.lexically_relative(engineRoot).generic_string();
			const std::filesystem::path solution =
				std::filesystem::path(WLD_REPO_ROOT) / WLD_OUTPUT_DIR / "World.slnx";
			if (!engineRelative.empty() && engineRelative.compare(0, 2, "..") != 0
				&& std::filesystem::is_regular_file(solution, fileError))
			{
				target = solution;
				mode = "solution";
			}
		}

		if (devenv.empty())
		{
			WLD_CORE_WARN("[vsopen] file={0} devenv= mode=none (Visual Studio not found via vswhere)",
				absPath.generic_string());
			report(Wui::Tr("notice.vsopen.no_devenv",
				"Visual Studio (devenv.exe) was not found via vswhere — install Visual Studio with the "
				"C++ workload, then try again."));
			return false;
		}
		// 固定日志(探针断言点):先按解析结果打一条,再决定真启动 / 只 dry-run。
		WLD_CORE_INFO("[vsopen] file={0} devenv={1} mode={2}", absPath.generic_string(),
			devenv.generic_string(), mode);

		const char* dryRun = std::getenv("WLD_VS_DRYRUN");
		if (dryRun != nullptr && dryRun[0] != '\0' && std::strcmp(dryRun, "0") != 0)
		{
			WLD_CORE_INFO("[vsopen] dry-run: not launching Visual Studio");
			report("dry-run: would open " + target.string() + " in " + devenv.string());
			return true;
		}

		std::wstring commandLine = L"\"" + devenv.wstring() + L"\" \"" + target.wstring() + L"\"";
		std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
		commandBuffer.push_back(L'\0');
		STARTUPINFOW startup {};
		startup.cb = sizeof(STARTUPINFOW);
		PROCESS_INFORMATION process {};
		const std::wstring workingDirectory = target.parent_path().wstring();
		const BOOL spawned = CreateProcessW(devenv.wstring().c_str(), commandBuffer.data(),
			nullptr, nullptr, FALSE, 0, nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
			&startup, &process);
		if (spawned)
		{
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
			report("opened in Visual Studio: " + target.string());
			return true;
		}

		const unsigned long lastError = GetLastError();
		WLD_CORE_WARN("[vsopen] CreateProcessW failed ({0}); falling back to ShellExecuteW", lastError);
		WLD_CORE_INFO("[vsopen] file={0} devenv={1} mode=shell", absPath.generic_string(),
			devenv.generic_string());
		const std::wstring parameters = L"\"" + target.wstring() + L"\"";
		const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open", devenv.wstring().c_str(),
			parameters.c_str(), nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
		{
			report("CreateProcessW failed (" + std::to_string(lastError) + ") and ShellExecuteW failed (code "
				+ std::to_string(static_cast<long long>(reinterpret_cast<INT_PTR>(shellResult)))
				+ ") for " + target.string());
			return false;
		}
		report("opened in Visual Studio (shell): " + target.string());
		return true;
	}

	bool EditorLayer::ScriptsCreateFromTemplate(std::string& outLogicalPath, std::string* message)
	{
		auto report = [message](const std::string& text)
		{
			if (message)
				*message = text;
		};
		const std::filesystem::path contentRoot = World::Paths::AssetRoot();
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
		for (const entt::entity handle : registry.view<LuauScriptComponent>())
		{
			if (scene.IsPendingDestroy(handle))
				continue;
			const auto& script = registry.get<LuauScriptComponent>(handle);
			if (!script.ScriptPath.empty())
				paths.push_back(script.ScriptPath);
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
			for (const entt::entity handle : registry.view<LuauScriptComponent>())
			{
				if (scene.IsPendingDestroy(handle))
					continue;
				const auto& probe = registry.get<LuauScriptComponent>(handle);
				if (probe.ScriptPath != path)
					continue;
				matched = true;
				// 可变组件引用:运行中的场景不能用非 const GetRegistry()(断言),走 Entity 的
				// 组件指针入口 —— 与属性面板读组件实例是同一条路径。
				Entity entity(target.get(), handle);
				auto* script = static_cast<LuauScriptComponent*>(
					entity.GetComponent(entt::type_id<LuauScriptComponent>().hash()));
				if (!script)
					continue;
				std::string message;
				const bool ok = ReloadLuauScriptComponent(*script, target.get(), &message);
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
			relative = std::filesystem::relative(documentPath, World::Paths::AssetRoot(), ec);
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

	// ---- HOTR-P2-T5:场景 `.wd` 自动重开(干净文档 + 编辑态)----
	//
	// 口径(方案 P2 细化设计,P2-a):外部改动经现有 150ms 消抖后,
	//   * 只走 RequestAction → 现有 DoOpenScene(**新 Scene 实例**,与手动"重新打开"同一条路径);
	//   * 重开前按 **UUID** 记选择、记编辑器相机(3D 轨道相机 + 视口 2D/3D 档),重开后恢复;
	//   * 文档 dirty → 保持现状(视口提示条 + 手动重开,**绝不**自动覆盖未保存修改);
	//   * Play/Simulate → 不自动;
	//   * 偏好 `scene_auto_reload`(默认开),`WLD_SCENE_AUTORELOAD=0` 覆盖(自动化用)。
	bool EditorLayer::SceneAutoReloadEnabled() const
	{
		// 环境变量 > 偏好文件(与 WLD_ASSET_HOTRELOAD 同一口径)。
		if (const char* switchValue = std::getenv("WLD_SCENE_AUTORELOAD"))
			return std::string(switchValue) != "0";
		return Editor::EditorPreferences::Get().Data().SceneAutoReload;
	}

	void EditorLayer::MaybeAutoReloadExternalScene()
	{
		if (!m_ExternalSceneChanged || !m_Document.HasPath())
			return;
		const std::string logical = CurrentDocumentLogicalPath();
		if (!SceneAutoReloadEnabled())
			return;   // 关掉开关 = 保持既有"提示 + 手动重开"行为
		if (m_SceneState != SceneState::Edit)
		{
			WLD_CORE_INFO("[asset-hot-reload] scene auto reload skipped '{0}' (play/simulate in progress)",
				logical);
			return;
		}
		if (m_Document.IsDirty())
		{
			// 有未保存修改:不动文档,视口提示条继续给"重新打开"(用户点它时才走未保存确认)。
			WLD_CORE_INFO("[asset-hot-reload] scene auto reload skipped '{0}' "
				"(unsaved edits; the reopen prompt is kept)", logical);
			return;
		}
		const std::filesystem::path target = m_Document.GetPath();
		CaptureSceneReopenSnapshot();
		WLD_CORE_INFO("[asset-hot-reload] scene auto reopening '{0}' (document clean, edit mode)", logical);
		RequestAction([this, target]() { DoOpenScene(target); });
	}

	void EditorLayer::CaptureSceneReopenSnapshot()
	{
		m_PendingSceneReopen = SceneReopenSnapshot {};
		m_PendingSceneReopen.Pending = true;
		// 选择:只记 UUID(重开后是新 Scene 实例,实体句柄会复用旧值 —— 按句柄恢复会选错实体)。
		const Scene* scene = m_ActiveScene.get();
		if (scene && m_SelectedEntity.IsValid() && m_SelectedEntity.GetScene() == scene
			&& m_SelectedEntity.HasComponent<UUIDComponent>())
		{
			m_PendingSceneReopen.HasSelection = true;
			m_PendingSceneReopen.SelectionUUID = m_SelectedEntity.GetComponent<UUIDComponent>().ID;
		}
		// 编辑器相机:3D 轨道相机可完整记录/恢复;2D EditorCamera 只暴露距离(其余是私有轨道量,
		// 不为此扩引擎接口)—— 恢复不了的部分在报告里写明。
		m_PendingSceneReopen.Viewport3D = m_Viewport3D;
		m_PendingSceneReopen.CameraTarget = m_EditorCamera3D.GetTarget();
		m_PendingSceneReopen.CameraDistance = m_EditorCamera3D.GetDistance();
		m_PendingSceneReopen.CameraYaw = m_EditorCamera3D.GetYaw();
		m_PendingSceneReopen.CameraPitch = m_EditorCamera3D.GetPitch();
		m_PendingSceneReopen.Camera2DDistance = m_EditorCamera.GetDistance();
		const glm::vec3& target = m_PendingSceneReopen.CameraTarget;
		WLD_CORE_INFO("[asset-hot-reload] scene reopen snapshot: selection={0} viewport3d={1} "
			"target=({2:.2f},{3:.2f},{4:.2f}) distance={5:.2f} yaw={6:.1f} pitch={7:.1f}",
			m_PendingSceneReopen.HasSelection ? "uuid" : "none", m_PendingSceneReopen.Viewport3D ? 1 : 0,
			target.x, target.y, target.z, m_PendingSceneReopen.CameraDistance,
			m_PendingSceneReopen.CameraYaw, m_PendingSceneReopen.CameraPitch);
	}

	void EditorLayer::RestoreSceneReopenSnapshot()
	{
		if (!m_PendingSceneReopen.Pending)
			return;
		const SceneReopenSnapshot snapshot = m_PendingSceneReopen;
		m_PendingSceneReopen = SceneReopenSnapshot {};   // 消费一次(普通打开不受影响)
		const Scene* scene = m_ActiveScene.get();
		Entity restored;
		if (snapshot.HasSelection && scene)
		{
			const entt::registry& registry = scene->GetRegistry();
			for (const entt::entity handle : registry.view<UUIDComponent>())
			{
				if (static_cast<uint64_t>(registry.get<UUIDComponent>(handle).ID)
					== static_cast<uint64_t>(snapshot.SelectionUUID))
				{
					restored = Entity(m_ActiveScene.get(), handle);
					break;
				}
			}
		}
		// 找不到(实体被删/改名/换 ID)= 清选择,不留指向旧场景的悬空选择。
		m_SelectedEntity = restored;
		// 相机 + 视口档:按快照写回(DoOpenScene 本身不碰相机,这里是显式的"恢复"语义)。
		m_Viewport3D = snapshot.Viewport3D;
		m_EditorCamera3D.SetTarget(snapshot.CameraTarget);
		m_EditorCamera3D.SetDistance(snapshot.CameraDistance);
		m_EditorCamera3D.SetYawPitch(snapshot.CameraYaw, snapshot.CameraPitch);
		m_EditorCamera.SetDistance(snapshot.Camera2DDistance);
		const glm::vec3& target = snapshot.CameraTarget;
		WLD_CORE_INFO("[asset-hot-reload] scene reopened '{0}' (selection={1}, camera restored: "
			"viewport3d={2} target=({3:.2f},{4:.2f},{5:.2f}) distance={6:.2f} yaw={7:.1f} pitch={8:.1f})",
			CurrentDocumentLogicalPath(),
			snapshot.HasSelection ? (restored.IsValid() ? "kept" : "cleared") : "none",
			snapshot.Viewport3D ? 1 : 0, target.x, target.y, target.z, snapshot.CameraDistance,
			snapshot.CameraYaw, snapshot.CameraPitch);
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
		if (!World::Asset::ImportFile(source, World::Paths::AssetRoot(), &imported, &error,
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
				std::filesystem::relative(logicalModel, World::Paths::AssetRoot(), ec);
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

		// HOTR-P1-T1:帧边界消费材质着色器热重载的后台编译产物(Install 只在主线程帧内执行;
		// 编译失败保留旧管线)。放在文档场景早退之前 —— 没有文档路径(新场景/启动器)时也生效。
		// 也放在热重载开关检查之后 —— 关掉热重载时不再装配在飞产物(与 Enqueue 同一道闸门)。
		m_ShaderHotReload.Pump();

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

		// HOTR-P1-T1:材质引用的着色器(`.slang`)改了 —— 面板没打开时也走编辑器级热重载:
		// 转交 ShaderHotReload(工作线程读源 + 编译),产物在下一帧边界 Pump 里 Install 到路径键。
		for (const std::string& path : report.ChangedShaders)
			m_ShaderHotReload.Enqueue(path);

		// HOTR-P2-T5(P2-c):内容根下 `.wtex` 及其 `source:` 源图的外部改动 → 自动重烘 `.wtexc`。
		// 与材质/贴图/场景共用上面那道"资产热重载"开关(WLD_ASSET_HOTRELOAD / 偏好);
		// Poll = 主线程指纹轮询 + 稳定窗口后派发(编码在工作线程),Pump = 主线程写盘 + 失效 + 日志。
		// **默认开启**(2026-09-30 修复后):`WLD_TEXTURE_HOTRELOAD=0` 可关(自动化/诊断用)。
		// 历史:翻默认值前实测到"活动材质换贴图 → 帧同步被打坏 → device lost";真因是
		// 材质贴图走异步上传环 + 重烘后材质未失效,两处都已修(见 plan.md P2-c 取证记录)。
		static const bool textureHotReloadEnabled = []
		{
			const char* value = std::getenv("WLD_TEXTURE_HOTRELOAD");
			return !(value != nullptr && *value != '\0' && std::string(value) == "0");
		}();
		if (textureHotReloadEnabled)
		{
			m_TextureImportWatch.Poll(static_cast<double>(deltaSeconds));
			m_TextureImportWatch.Pump();
		}

		// HOTR-P2-T6(P2-b):当前文档场景引用的 `.wprefab` 外部改动 → 帧边界安全点逐实例跟随
		// (保留 override;失败只记日志,其余实例继续)。放在文档路径早退之前:场景文档在内容根
		// 之外时,实例引用的 prefab 逻辑路径仍然有效,跟随不受影响。
		PollPrefabHotReload(static_cast<double>(deltaSeconds));

		// 2) 文档场景(.wd):内容变化 → 视口提示条;**干净文档 + 编辑态**再自动重开
		//    (HOTR-P2-T5;重开前按 UUID 记选择、记相机,重开后恢复)。
		//    dirty / Play / Simulate / `scene_auto_reload` 关闭 → 只提示 + 手动重开,
		//    绝不自动覆盖未保存修改。
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
			// HOTR-P2-T5:干净文档 + 编辑态 → 走 RequestAction 自动重开(选择/相机恢复);
			// dirty / Play / Simulate / 开关关闭 → 这一条不做任何事(保留上面的提示语义)。
			MaybeAutoReloadExternalScene();
		}
	}

	// ---- HOTR-P2-T6:`.wprefab` 实例跟随(Edit 态 + 150ms 消抖)----
	//
	// 口径(方案 P2 细化设计,P2-b):
	//   * 监听集合 = 当前**文档场景** PrefabInstances() 里的来源路径(每帧同步:新引用 Watch、
	//     消失的引用 Unwatch);Play/Simulate 的活动场景是运行时副本,不跟随;
	//   * 变化经 AssetFileWatch 150ms 消抖后进入未决集合,只在 CanApplyScriptReload()
	//     的安全点消费(ApplyPrefabChanges 会改实体组件;与脚本热重载同一安全点口径),
	//     不安全时顺延到下一帧,不丢;
	//   * 每个被引用实例单独调用 Gameplay::ApplyPrefabChanges:false = 该实例未改(记 failed,
	//     其余实例继续);true + 非空 error = 已应用但有字段跳过(记 WARN,不当作失败)。
	void EditorLayer::PollPrefabHotReload(float deltaSeconds)
	{
		Scene* scene = m_ActiveScene.get();
		const bool editDocument = scene != nullptr && m_SceneState == SceneState::Edit
			&& scene == m_Document.GetScene().get();

		// 1) 监听集合同步(编辑态 = 当前实例引用的路径;其它形态清空,回到编辑态时重新建立基线)。
		std::vector<std::string> wanted;
		if (editDocument)
		{
			for (const Gameplay::PrefabInstanceRecord& record : scene->PrefabInstances())
			{
				if (record.PrefabPath.empty())
					continue;
				if (std::find(wanted.begin(), wanted.end(), record.PrefabPath) == wanted.end())
					wanted.push_back(record.PrefabPath);
			}
		}
		for (const std::string& watched : m_PrefabWatch.WatchedPaths())
		{
			if (std::find(wanted.begin(), wanted.end(), watched) == wanted.end())
				m_PrefabWatch.Unwatch(watched);
		}
		for (const std::string& path : wanted)
		{
			if (!m_PrefabWatch.IsWatched(path))
			{
				m_PrefabWatch.Watch(path);
				WLD_CORE_INFO("[asset-hot-reload] prefab watch: {0} prefab(s)", m_PrefabWatch.Size());
			}
		}

		// 2) 上一帧顺延下来的变化先消费(可能没有;安全点满足才真正应用)。
		ApplyPendingPrefabChanges();

		// 3) 轮询:稳定变化进入未决集合(报告后该内容即成为新基线,靠未决集合保证不丢),
		//    安全点满足时立即应用,否则下一帧再试。
		for (const std::string& changed : m_PrefabWatch.Poll(static_cast<double>(deltaSeconds)))
		{
			if (std::find(m_PendingPrefabReloads.begin(), m_PendingPrefabReloads.end(), changed)
				== m_PendingPrefabReloads.end())
				m_PendingPrefabReloads.push_back(changed);
		}
		ApplyPendingPrefabChanges();
	}

	void EditorLayer::ApplyPendingPrefabChanges()
	{
		if (m_PendingPrefabReloads.empty())
			return;
		Scene* scene = m_ActiveScene.get();
		if (scene == nullptr || m_SceneState != SceneState::Edit || scene != m_Document.GetScene().get())
			return;   // Play/Simulate / 无文档:不消费(回到编辑态后按新基线重新登记)
		if (!scene->CanApplyScriptReload())
			return;   // 不安全点(脚本回调/结构提交中):顺延,下一帧再试

		std::vector<std::string> pending;
		pending.swap(m_PendingPrefabReloads);
		for (const std::string& path : pending)
		{
			// 变化期间实例可能被删/改来源:以当前注册表为准重新匹配,不用报告时的快照。
			std::vector<entt::entity> roots;
			for (const Gameplay::PrefabInstanceRecord& record : scene->PrefabInstances())
				if (record.PrefabPath == path)
					roots.push_back(record.Root);
			if (roots.empty())
				continue;   // 已无实例引用它(例如实例刚被断开链接)

			uint32_t applied = 0;
			std::string skippedFields;
			for (const entt::entity root : roots)
			{
				Gameplay::PrefabInstanceRecord* record = scene->FindPrefabInstance(root);
				if (!record)
					continue;
				std::string error;
				if (!Gameplay::ApplyPrefabChanges(*record, *scene, &error))
				{
					// false + 非空 error = 该实例一个实体都没改(读档失败/结构不符):记日志,其余继续。
					WLD_CORE_WARN("[asset-hot-reload] prefab failed '{0}': {1}", path,
						error.empty() ? std::string("unknown error") : error);
					continue;
				}
				++applied;
				// true + 非空 error = 已应用但有字段跳过(如覆盖记录指向的字段已失效):WARN,不算失败。
				if (!error.empty() && skippedFields.empty())
					skippedFields = error;
			}
			if (applied > 0)
			{
				WLD_CORE_INFO("[asset-hot-reload] prefab applied '{0}' instances={1}", path, applied);
				if (!skippedFields.empty())
					WLD_CORE_WARN("[asset-hot-reload] prefab applied '{0}' with skipped field(s): {1}",
						path, skippedFields);
			}
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
					const fs::path projectManifestPath = World::Paths::ProjectFile("project.we.yaml");
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


