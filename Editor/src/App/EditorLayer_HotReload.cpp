#include "EditorLayer_Internal.h"
#include "World/Asset/AssetCatalog.h"

namespace World
{

using namespace EditorLayerDetail;


	// ---- P2 W8:Scripts 面板的宿主能力 ----

bool EditorLayer::ScriptsReloadInstance(entt::entity handle, std::string* message){
		(void)handle;
		if (message)
			*message = "scene script instance reload is deprecated in pure ECS";
		return false;
	}


bool EditorLayer::ScriptsOpenExternal(const std::string& logicalPath, std::string* message){
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

namespace EditorLayerDetail
{
		// CPPT-7/PROJ-8:vswhere → devenv.exe。会话内只解析一次(VS 安装不会在编辑会话中途变化),
		// 结果空 = 没找到(调用方给可读提示,不静默走系统文件关联)。
const std::filesystem::path& VisualStudioDevenvPath(){
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

bool EditorLayer::OpenInVisualStudio(const std::filesystem::path& absPath, std::string* message){
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


bool EditorLayer::ScriptsCreateFromTemplate(std::string& outLogicalPath, std::string* message){
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
		// 落点 = scripts/systems/ —— 只有这个目录下的脚本会被场景启动时自动加载执行。
		for (int index = 1; index <= 10000; ++index)
		{
			const std::string logicalPath = "scripts/systems/script_" + std::to_string(index) + ".luau";
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
				report("created " + logicalPath + " (from templates/WorldScript.lua; auto-loaded under scripts/systems/)");
				WLD_CORE_INFO("[scripts-panel] created script '{0}' from template", logicalPath);
				return true;
			}
			if (copyError == std::make_error_condition(std::errc::file_exists))
				continue;   // 竞态:刚被别处创建 → 名字递增重试
			report("create failed for " + target.string() + ": " + copyError.message());
			return false;
		}
		report("could not find a free scripts/systems/script_<n>.luau name under " + contentRoot.string());
		return false;
	}


void EditorLayer::PollScriptHotReload(float deltaSeconds){
		(void)deltaSeconds;
	}


std::string EditorLayer::CurrentDocumentLogicalPath() const{
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


void EditorLayer::RebaselineExternalSceneWatch(){
		const std::string logical = CurrentDocumentLogicalPath();
		m_SceneWatch.Clear();
		m_WatchedSceneLogicalPath.clear();
		m_ExternalSceneChanged = false;
		if (logical.empty())
			return;
		m_SceneWatch.Watch(logical);
		m_WatchedSceneLogicalPath = logical;
	}


void EditorLayer::ReopenExternalScene(){
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
bool EditorLayer::SceneAutoReloadEnabled() const{
		// 环境变量 > 偏好文件(与 WLD_ASSET_HOTRELOAD 同一口径)。
		if (const char* switchValue = std::getenv("WLD_SCENE_AUTORELOAD"))
			return std::string(switchValue) != "0";
		return Editor::EditorPreferences::Get().Data().SceneAutoReload;
	}


void EditorLayer::MaybeAutoReloadExternalScene(){
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


void EditorLayer::CaptureSceneReopenSnapshot(){
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


void EditorLayer::RestoreSceneReopenSnapshot(){
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


bool EditorLayer::InstantiateModelFile(const std::string& logicalPath, std::string* message){
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


bool EditorLayer::ImportModelFile(const std::string& sourcePath, std::string* message, std::string* outLogicalModel, const std::string& destinationLogicalDir){
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
		// 导入产出的 .wmodel/.wmat 立刻进资产目录 —— 不重扫内容根,只登记这一批文件。
		// 之后即使有人改名/移动它们,"按身份找回"也已经能命中。
		if (m_ActiveScene)
		{
			WorldContext& context = m_ActiveScene->GetContext();
			World::RefreshAssetCatalog(context, logicalModel);
			for (const std::string& material : imported.MaterialPaths)
				World::RefreshAssetCatalog(context, material);
		}
		WLD_CORE_INFO("[model] {0}", text);
		return true;
	}


void EditorLayer::ImportModelDialog(){
		std::string path = FileDialogs::OpenFile(
			"glTF Model (*.gltf;*.glb)\0*.gltf;*.glb\0All Files (*.*)\0*.*\0");
		if (path.empty())
			return;
		// D10(用户 2026-09-19):选完源文件后,导入位置由**窗口级居中模态**里的树状选择器选
		// (范围限定在内容根内,不再用原生文件夹对话框 —— 后者可能选到工作区外,
		// 那种位置场景与打包都引用不到)。模态把结果交回同一条导入路径。
		m_Shell.RequestImportDestination(path);
	}


void EditorLayer::PollAssetHotReload(float deltaSeconds){
		// 开关(环境变量 > 偏好文件):
		//  - WLD_ASSET_HOTRELOAD=0 整体关闭(默认开;自动化脚本用);
		//  - 否则读编辑器偏好"资产热重载"(P4-UX7:以前只能靠环境变量,现在有面板入口)。
		if (!AssetHotReloadEnabled())
			return;

		// HOTR-P3-T10:资产侧在飞产物的统一主线程提交入口(材质 `.slang` 的 Install、`.wtex`
		// 重烘落盘 + 缓存失效、glTF 重导入原子替换;各服务内部保持自己的子开关与日志口径)。
		// 放在文档场景早退之前 —— 没有文档路径(新场景/启动器)时也生效;也放在热重载开关
		// 检查之后 —— 关掉热重载时不再装配在飞产物(与 Enqueue 同一道闸门)。
		m_HotReloadHost.Pump(true);

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
		// 转交宿主(工作线程读源 + 编译),产物在下一帧边界宿主 Pump 里 Install 到路径键。
		for (const std::string& path : report.ChangedShaders)
			m_HotReloadHost.EnqueueShader(path);

		// HOTR-P3-T10:`.wtex` 自动重烘(T5)与 glTF 自动重导入(T9)的轮询/提交已收敛进
		// `m_HotReloadHost`:Poll 在 OnUpdate 的帧边界统一入口(活动场景 + 渲染器 + 热重载
		// 开关都满足时),提交在上面那次宿主 Pump;`WLD_TEXTURE_HOTRELOAD=0` /
		// `WLD_MODEL_HOTRELOAD=0` 子开关语义不变(在宿主内部判定)。

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
	//   * 变化经 AssetFileWatch 150ms 消抖后进入未决集合,只在 CanApplyModuleReload()
	//     的安全点消费(ApplyPrefabChanges 会改实体组件;与脚本热重载同一安全点口径),
	//     不安全时顺延到下一帧,不丢;
	//   * 每个被引用实例单独调用 Gameplay::ApplyPrefabChanges:false = 该实例未改(记 failed,
	//     其余实例继续);true + 非空 error = 已应用但有字段跳过(记 WARN,不当作失败)。
void EditorLayer::PollPrefabHotReload(float deltaSeconds){
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


void EditorLayer::ApplyPendingPrefabChanges(){
		if (m_PendingPrefabReloads.empty())
			return;
		Scene* scene = m_ActiveScene.get();
		if (scene == nullptr || m_SceneState != SceneState::Edit || scene != m_Document.GetScene().get())
			return;   // Play/Simulate / 无文档:不消费(回到编辑态后按新基线重新登记)
		if (!scene->CanApplyModuleReload())
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


void EditorLayer::SetSceneState(SceneState state){
		if (m_SceneState == state) return;

		// End the previous mode before replacing any scene references.
		if (m_RuntimeScene)
		{
			// P2 W4:Play 与 Simulate 都由 PlayHost(GameApp 会话)持有运行时生命周期。
			if (m_SceneState == SceneState::Play || m_SceneState == SceneState::Simulate)
			{
				m_PlayHost.StopRuntime();
				m_PlayHost.Shutdown();
				// GameUI(M7b):Play/Simulate 结束 —— 停画游戏 UI 并清掉它在无障碍树里的登记,
				// 避免上一帧的节点在退出 Play 后变成幽灵行(共享通道 ⇒ 不关编辑器通道)。
				m_UiHost.Shutdown();
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
			// GameUI(M7b):Play/Simulate 期间接上当前项目的游戏 UI(`WLD_UI_DOC` → 内容根
			// `assets/ui/*.wui`;都没有 = 静默关闭)。物理面/原点每帧由 OnUiFrame 按视口面板的
			// 场景矩形设置;节点登记进编辑器同一份无障碍树(panel = 文档 Screen 名)。
			m_UiHost.Initialize(World::Paths::AssetRoot());
			// GameUI(M11):世界空间 UI 的默认位置解析器(实体名 → 世界位置)每次 Play 只设一次 ——
			// `std::function` 不每帧重建;解析器内部按 tick 缓存实体名索引。相机每帧喂(见
			// DrawPlayModeGameUi)。
			if (m_UiHost.Enabled())
				m_UiHost.SetWorldPositionResolver(m_PlayHost.GetWorldPositionResolver());
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

void EditorLayer::UpdateSceneContext(Ref<Scene> scene){
		m_HasRenderedScene = false;
		m_ActiveScene = scene;
		if (m_ActiveScene && m_ViewportSize.x > 0.0f && m_ViewportSize.y > 0.0f)
		{
			m_ActiveScene->OnViewportResize((uint32_t)m_ViewportSize.x, (uint32_t)m_ViewportSize.y);
		}
	}


void EditorLayer::RequestAction(std::function<void()> action){
		if (m_Document.IsDirty())
		{
			m_PendingAction = std::move(action);
			m_ShowUnsavedModal = true;
			return;
		}
		action();
	}


void EditorLayer::ShowError(const std::string& message){
		m_ErrorText = message;
		m_ShowErrorModal = true;
		WLD_CORE_ERROR("{0}", message);
	}




void EditorLayer::StartCooking(const std::string& target){
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
