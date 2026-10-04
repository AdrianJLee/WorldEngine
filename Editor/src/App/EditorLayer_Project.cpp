#include "EditorLayer_Internal.h"

namespace World
{

using namespace EditorLayerDetail;


	// ---- CPPT-3:Game 模块(`Game.dll`)热重载(编辑器入口;plan CPPT-2 §6.1 T5b)----

const char* EditorLayer::CppModuleStateName() const{
		switch (m_CppModuleStatus.State)
		{
			case CppModuleState::Loaded: return "loaded";
			case CppModuleState::Unloaded: return "unloaded";
			case CppModuleState::Reloading: return "reloading";
			case CppModuleState::RolledBack: return "rolled-back";
		}
		return "unloaded";
	}


bool EditorLayer::IsCppModuleLoaded() const{
		// 引擎实况(按模块 id 查当前模块表):重载第一段完成后这里立刻为 false,
		// `reloading` 窗口因此天然落进"未加载"分支(存根门禁、C++ 脚本下拉的未注册提示)。
		return !Modules::GameModuleReload::IsUnloaded(Application::Get().GetContext());
	}


void EditorLayer::PublishCppModuleResult(const Modules::GameModuleReloadResult& result, CppModuleState state, bool ok){
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


bool EditorLayer::UnloadCppModule(std::string* message){
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


bool EditorLayer::ReloadCppModule(std::string* message){
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
bool EditorLayer::BuildAndReloadCppModule(std::string* message){
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
void EditorLayer::PollCppModuleBuild(){
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
void EditorLayer::PollPluginBuild(){
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


std::string EditorLayer::CppModuleStatusJson() const{
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


void EditorLayer::CloseAction(){
		RequestAction([this]() { World::Application::Get().Close(); });
	}


	// PROJ-3/T1:关闭启动器 = 退出进程(启动器模式没有"当前项目"可停留)。
	// 启动器页的"退出"按钮、Esc 与主窗口关闭事件(Alt+F4 / WM_CLOSE)共用这一条;
	// 日志固定一行,只写一次(两条路径可能在同一帧先后到达)。
void EditorLayer::RequestLauncherExit(){
		if (!m_LauncherExitLogged)
		{
			m_LauncherExitLogged = true;
			WLD_CORE_INFO("[launcher] closed; exiting");
		}
		World::Application::Get().Close();
	}


void EditorLayer::DuplicateSelectedEntity(){
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


void EditorLayer::ApplyRendererChange(const std::string& name){
		if (!Renderer::SetRequestedRenderer(name))
			return;
		// 运行中热切换会串资源(GL 名字/描述符集跨上下文复用 → 图标与贴图错乱),
		// 改为自动重启编辑器进程:设置已写入清单,重启后即按新后端干净启动。
		RestartForRendererChange();
	}


void EditorLayer::RestartForRendererChange(){
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
void EditorLayer::RelaunchWithProject(const std::filesystem::path& projectRoot){
		if (projectRoot.empty())
			return;
		RequestAction([this, projectRoot]() { DoRelaunchWithProject(projectRoot); });
	}


void EditorLayer::DoRelaunchWithProject(const std::filesystem::path& projectRoot){
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
void EditorLayer::DismissProjectLauncher(){
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


void EditorLayer::LaunchProjectRuntime(const std::filesystem::path& projectRoot){
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
void EditorLayer::AlignEditorCamera3DWithView(){
		m_EditorCamera3D.SetYawPitch(180.0f, 0.0f);
		m_EditorCamera3D.FocusOn(glm::vec3(0.0f), 12.0f);
	}


void EditorLayer::ProcessPendingRendererChange(){
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


void EditorLayer::RegisterUiTextures(){
		auto& registry = Wui::WuiTextureRegistry::Get();
		if (m_SceneRenderer && m_SceneRenderer->GetColorTexture())
			m_SceneTextureId = registry.Register(m_SceneRenderer->GetColorTexture());
		if (m_PreviewRenderer && m_PreviewRenderer->GetColorTexture())
			m_PreviewTextureId = registry.Register(m_PreviewRenderer->GetColorTexture());
		// 图标也走唯一的 GPU 驻留:一次解码、一份显存、一个登记入口(不再有 GL 侧副本)。
		const Rhi::Handle<Rhi::Texture> icons[8] = {
			m_IconPlay, m_IconStop, m_IconPause, m_IconContinue,
			m_IconSimulate, m_IconSimulateStop, m_IconSimulatePause, m_IconSimulateContinue,
		};
		for (int i = 0; i < 8; ++i)
			m_IconIds[i] = registry.Register(icons[i]);
		m_UiTextureGeneration = registry.Generation();
	}


uint64_t EditorLayer::GetIconId(int index) const{
		if (index < 0 || index >= 8)
			return 0;
		return m_IconIds[index];
	}


void EditorLayer::TogglePlay(){
		SetSceneState(m_SceneState == SceneState::Play ? SceneState::Edit : SceneState::Play);
	}


void EditorLayer::ToggleSimulate(){
		SetSceneState(m_SceneState == SceneState::Simulate ? SceneState::Edit : SceneState::Simulate);
	}


void EditorLayer::TogglePause(){
		if (m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate)
		{
			m_ScenePaused = !m_ScenePaused;
			// P2 W4:暂停时仍在帧内 Tick 会话(帧末事件派发),但固定步长(计时器/系统)与
			// 可变阶段由 GameApp 的暂停标志冻结,二者必须同步。
			if (Gameplay::GameApp* app = Gameplay::GameApp::TryGet())
				app->SetPaused(m_ScenePaused);
		}
	}

void EditorLayer::SetViewportState(bool focused, bool hovered, glm::vec2 size, glm::vec2 bounds[2]){
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


void EditorLayer::ResolveUnsavedModal(bool save){
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


void EditorLayer::CancelUnsavedModal(){
		m_PendingAction = nullptr;
		m_ShowUnsavedModal = false;
	}


bool EditorLayer::TrySave(){
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

bool EditorLayer::OnKeyPressed(KeyPressedEvent& e){
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


glm::mat4 EditorLayer::EntityWorldMatrix(Entity entity){
		glm::mat4 world = entity.GetComponent<TransformComponent>().GetLocalMatrix();
		if (entity.HasComponent<WorldTransformComponent>())
			world = entity.GetComponent<WorldTransformComponent>().Matrix;
		return world;
	}


Entity EditorLayer::GetPreviewCameraEntity() const{
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


void EditorLayer::RenderCameraPreview(){
		if (!m_PreviewRenderer || !m_ActiveScene)
			return;
		Entity cameraEntity = GetPreviewCameraEntity();
		if (!cameraEntity.IsValid())
			return;
		// 与主视口完全同一条提交路径(同一套 SceneRenderer/管线/相机数据),因此
		// "预览分辨率 = 视口分辨率"时,预览图就是该相机看到的画面(验收用的等式)。
		// PECS(相机):组件只存参数,投影矩阵走 CameraSystem 的指纹缓存(编辑态读时懒建视图)。
		const Camera& camera = m_ActiveScene->GetCameraView(static_cast<entt::entity>(cameraEntity));
		const glm::mat4 world = EntityWorldMatrix(cameraEntity);
		m_PreviewRenderer->BeginScene(m_ActiveScene.get(), m_RendererOptions);
		m_PreviewRenderer->SubmitScene(camera, world);
		m_PreviewRenderer->EndScene();
	}


std::string EditorLayer::CameraPreviewLabel() const{
		Entity cameraEntity = GetPreviewCameraEntity();
		if (!cameraEntity.IsValid())
			return {};   // 空串 = 当前没有相机预览(面板据此隐藏小窗)
		if (cameraEntity.HasComponent<TagComponent>())
		{
			const std::string& tag = StringPool::Get().NameOf(cameraEntity.GetComponent<TagComponent>().Tag);
			if (!tag.empty())
				return tag;
		}
		if (m_ActiveScene && cameraEntity == m_ActiveScene->GetPrimaryCameraEntity())
			return "(primary camera)";
		return "(camera)";
	}


Entity EditorLayer::GetEntityAtMousePosition(glm::vec2 viewportLocal){
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

}
