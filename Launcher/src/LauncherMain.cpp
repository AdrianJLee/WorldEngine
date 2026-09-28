// PROJ-3/T2:项目侧小启动器 —— WeEdit.exe / WePlay.exe。
//
// 目的:把这两个 exe 单独复制进任意项目目录,双击就能用本机引擎打开该项目:
//   WeEdit.exe ⇒ <引擎根>/<WLD_OUTPUT_DIR>Editor/<WLD_BUILD_TYPE>/Editor.exe  --project <exe 所在目录>
//   WePlay.exe ⇒ <引擎根>/<WLD_OUTPUT_DIR>Runtime/<WLD_BUILD_TYPE>/Runtime.exe --project <exe 所在目录>
//
// 硬性约束(方案 v2 的 P2a):
//  * 零引擎依赖:不链接 World/Game,进程不加载 WorldRuntime.dll ⇒ 只依赖系统 DLL,可单独复制。
//  * 项目根 = exe 自身所在目录;命令行显式 --project <dir> / --project=<dir> 优先。
//  * 工作目录 = 项目根(目标 exe 按 CWD 解析项目清单与内容根)。
//  * GUI 子系统:双击不弹控制台;失败(缺清单 / 缺目标 exe / 拉起失败)一律 MessageBoxW。
//
// 两个目标共用这份源码,模式由编译宏区分(见 Launcher/CMakeLists.txt):
//   WLD_LAUNCH_MODE_EDIT / WLD_LAUNCH_MODE_PLAY。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <string>
#include <vector>

#if !defined(WLD_LAUNCH_MODE_EDIT) && !defined(WLD_LAUNCH_MODE_PLAY)
#error "LauncherMain.cpp:必须定义 WLD_LAUNCH_MODE_EDIT 或 WLD_LAUNCH_MODE_PLAY 之一"
#endif
#if defined(WLD_LAUNCH_MODE_EDIT) && defined(WLD_LAUNCH_MODE_PLAY)
#error "LauncherMain.cpp:WLD_LAUNCH_MODE_EDIT 与 WLD_LAUNCH_MODE_PLAY 只能二选一"
#endif
#if !defined(WLD_REPO_ROOT) || !defined(WLD_OUTPUT_DIR) || !defined(WLD_BUILD_TYPE)
#error "LauncherMain.cpp:缺少 WLD_REPO_ROOT / WLD_OUTPUT_DIR / WLD_BUILD_TYPE(见 Launcher/CMakeLists.txt)"
#endif

namespace
{
#if defined(WLD_LAUNCH_MODE_EDIT)
	constexpr wchar_t kLauncherName[] = L"WeEdit.exe";
	constexpr wchar_t kTargetFolder[] = L"Editor";      // <WLD_OUTPUT_DIR> 下的模块目录
	constexpr wchar_t kTargetExeName[] = L"Editor.exe";
	constexpr wchar_t kTargetCMakeName[] = L"Editor";   // cmake --build … --target
	constexpr wchar_t kTargetDescription[] = L"编辑器";
#else
	constexpr wchar_t kLauncherName[] = L"WePlay.exe";
	constexpr wchar_t kTargetFolder[] = L"Runtime";
	constexpr wchar_t kTargetExeName[] = L"Runtime.exe";
	constexpr wchar_t kTargetCMakeName[] = L"Runtime";
	constexpr wchar_t kTargetDescription[] = L"运行时";
#endif

	// 退出码(自动化探针按码判断,不要只看弹窗文本):
	// 0 = 已拉起;1 = 命令行/项目目录参数有问题;2 = 项目里没有 project.we.yaml;
	// 3 = 引擎根或目标 exe 缺失;4 = CreateProcess 与 ShellExecute 都失败。
	constexpr int kExitOk = 0;
	constexpr int kExitBadArguments = 1;
	constexpr int kExitNoManifest = 2;
	constexpr int kExitMissingTarget = 3;
	constexpr int kExitLaunchFailed = 4;

	void ShowMessage(const std::wstring& text, UINT icon)
	{
		// MB_SETFOREGROUND:从资源管理器双击时保证弹窗在前台,不会被别的窗口盖住。
		MessageBoxW(nullptr, text.c_str(), L"WorldEngine 启动器", MB_OK | MB_SETFOREGROUND | icon);
	}

	// CMake 注入的字面量是 UTF-8(/utf-8);这里统一转成 UTF-16,不假定路径是 ASCII。
	std::wstring WideFromUtf8(const char* text)
	{
		if (!text || !*text)
			return {};
		const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
		if (length <= 0)
			return {};
		std::wstring wide(static_cast<size_t>(length), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
		wide.pop_back();   // -1 起算的长度带上结尾 '\0',去掉。
		return wide;
	}

	// 归一化成 Windows 路径:正斜杠(CMake 的 CMAKE_SOURCE_DIR)→ 反斜杠;去掉尾分隔符。
	// 去掉尾分隔符是硬要求 —— `--project "D:\proj\"` 里的 `\"` 会被 MSVC CRT 解析成字面引号。
	std::wstring NormalizePath(std::wstring path)
	{
		for (wchar_t& character : path)
		{
			if (character == L'/')
				character = L'\\';
		}
		while (!path.empty() && path.back() == L'\\')
			path.pop_back();
		return path;
	}

	std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf)
	{
		return base.empty() ? leaf : base + L"\\" + leaf;
	}

	// exe 自身所在目录 = 默认项目根(方案:项目根 = exe 自身所在目录)。
	std::wstring ModuleDirectory()
	{
		std::wstring buffer(MAX_PATH, L'\0');
		for (;;)
		{
			const DWORD written = GetModuleFileNameW(nullptr, buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (written == 0)
				return {};
			if (written < buffer.size())
			{
				buffer.resize(written);
				break;
			}
			buffer.resize(buffer.size() * 2);   // 截断 ⇒ 翻倍重试(长路径)。
		}
		const size_t separator = buffer.find_last_of(L"\\/");
		if (separator == std::wstring::npos)
			return {};
		return buffer.substr(0, separator);
	}

	bool IsDirectory(const std::wstring& path)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	bool IsRegularFile(const std::wstring& path)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	bool EqualsIgnoreCase(const std::wstring& text, const wchar_t* other)
	{
		return CompareStringOrdinal(text.c_str(), -1, other, -1, TRUE) == CSTR_EQUAL;
	}

	bool StartsWithIgnoreCase(const std::wstring& text, const wchar_t* prefix)
	{
		const size_t prefixLength = std::wcslen(prefix);
		if (text.size() < prefixLength)
			return false;
		return CompareStringOrdinal(text.c_str(), static_cast<int>(prefixLength), prefix,
			static_cast<int>(prefixLength), TRUE) == CSTR_EQUAL;
	}

	struct ParsedCommandLine
	{
		std::wstring project;                    // --project 显式指定(空 = 用 exe 所在目录)
		std::vector<std::wstring> passthrough;   // 其余参数原样转交目标 exe
		bool projectFlagMissingValue = false;    // 只写了 --project 但后面没有目录
	};

	ParsedCommandLine ParseCommandLine()
	{
		ParsedCommandLine parsed;
		int argumentCount = 0;
		LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
		if (!arguments)
			return parsed;

		for (int index = 1; index < argumentCount; ++index)
		{
			const std::wstring argument = arguments[index];
			if (EqualsIgnoreCase(argument, L"--project"))
			{
				if (index + 1 >= argumentCount || arguments[index + 1][0] == L'\0')
					parsed.projectFlagMissingValue = true;
				else
					parsed.project = arguments[++index];
				continue;
			}
			if (StartsWithIgnoreCase(argument, L"--project="))
			{
				const std::wstring value = argument.substr(std::wcslen(L"--project="));
				if (value.empty())
					parsed.projectFlagMissingValue = true;
				else
					parsed.project = value;
				continue;
			}
			parsed.passthrough.push_back(argument);
		}

		LocalFree(arguments);
		return parsed;
	}

	// MSVC CRT 规则的反向引用:只有当参数里有空白/引号时才加引号,并转义反斜杠。
	// 透传参数是从 CommandLineToArgvW 拆出来的,重新拼命令行必须自己补引用。
	std::wstring QuoteArgument(const std::wstring& argument)
	{
		if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
			return argument;

		std::wstring quoted = L"\"";
		size_t backslashes = 0;
		for (const wchar_t character : argument)
		{
			if (character == L'\\')
			{
				++backslashes;
				continue;
			}
			if (character == L'"')
			{
				quoted.append(backslashes * 2 + 1, L'\\');
				quoted.push_back(L'"');
				backslashes = 0;
				continue;
			}
			quoted.append(backslashes, L'\\');
			backslashes = 0;
			quoted.push_back(character);
		}
		quoted.append(backslashes * 2, L'\\');
		quoted.push_back(L'"');
		return quoted;
	}

	// 自定义环境块:沿用当前环境,覆盖 WLD_PROJECT_DIR(与 EditorLayer::DoRelaunchWithProject
	// 同一口径:旧的 WLD_PROJECT_DIR / WLD_START_SCENE 不带进新项目)。
	std::wstring BuildEnvironmentBlock(const std::wstring& projectRoot)
	{
		std::wstring environmentBlock;
		if (LPWCH current = GetEnvironmentStringsW())
		{
			for (const wchar_t* entry = current; *entry; entry += std::wcslen(entry) + 1)
			{
				if (std::wcsncmp(entry, L"WLD_PROJECT_DIR=", 16) == 0)
					continue;
				if (std::wcsncmp(entry, L"WLD_START_SCENE=", 16) == 0)
					continue;
				environmentBlock.append(entry);
				environmentBlock.push_back(L'\0');
			}
			FreeEnvironmentStringsW(current);
		}
		environmentBlock.append(L"WLD_PROJECT_DIR=");
		environmentBlock.append(projectRoot);
		environmentBlock.push_back(L'\0');
		environmentBlock.push_back(L'\0');
		return environmentBlock;
	}
}

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previousInstance*/,
	LPWSTR /*commandLine*/, int /*showCommand*/)
{
	// 1) 项目根:--project 显式优先,否则 = exe 自身所在目录。
	const ParsedCommandLine parsed = ParseCommandLine();
	if (parsed.projectFlagMissingValue)
	{
		ShowMessage(std::wstring(L"--project 后面需要一个项目目录,例如:\n\n  ") + kLauncherName +
			L" --project \"D:\\MyGame\"", MB_ICONERROR);
		return kExitBadArguments;
	}

	std::wstring projectRoot = NormalizePath(parsed.project);
	if (projectRoot.empty())
		projectRoot = NormalizePath(ModuleDirectory());
	if (projectRoot.empty())
	{
		ShowMessage(std::wstring(L"无法确定项目目录:") + kLauncherName +
			L" 应该放在项目根目录里双击运行,或用 --project <目录> 显式指定。", MB_ICONERROR);
		return kExitBadArguments;
	}
	if (!IsDirectory(projectRoot))
	{
		ShowMessage(L"项目目录不存在:\n  " + projectRoot, MB_ICONERROR);
		return kExitBadArguments;
	}

	// 2) 项目清单自检:这个 exe 是"项目自己的入口",目录里必须有标准清单。
	if (!IsRegularFile(JoinPath(projectRoot, L"project.we.yaml")))
	{
		ShowMessage(L"这个目录里没有 project.we.yaml:\n  " + projectRoot +
			L"\n\n把 " + kLauncherName + L" 放进项目根目录双击运行," +
			L"或用 --project <目录> 显式指定项目。", MB_ICONWARNING);
		return kExitNoManifest;
	}

	// 3) 目标 exe:定位口径与 EditorLayer/EditorCooker 的开发布局完全一致 ——
	//    <引擎根>/<WLD_OUTPUT_DIR><模块>/<WLD_BUILD_TYPE>/<目标>.exe(编译期宏带尾分隔符)。
	const std::wstring repoRoot = NormalizePath(WideFromUtf8(WLD_REPO_ROOT));
	const std::wstring outputDirectory = NormalizePath(WideFromUtf8(WLD_OUTPUT_DIR));
	const std::wstring buildType = NormalizePath(WideFromUtf8(WLD_BUILD_TYPE));
	const std::wstring targetDirectory =
		JoinPath(JoinPath(JoinPath(repoRoot, outputDirectory), kTargetFolder), buildType);
	const std::wstring targetExe = JoinPath(targetDirectory, kTargetExeName);
	const std::wstring runtimeDll = JoinPath(targetDirectory, L"WorldRuntime.dll");

	const std::wstring buildCommand =
		L"cmake --build " + outputDirectory + L" --config " + buildType +
		L" --target " + kTargetCMakeName;

	if (!IsDirectory(repoRoot))
	{
		ShowMessage(L"找不到引擎根(编译期写死):\n  " + repoRoot +
			L"\n\n" + kLauncherName + L" 需要从构建它的那台引擎仓库里运行;" +
			L"换了机器/移动过仓库时,请重新构建启动器。", MB_ICONERROR);
		return kExitMissingTarget;
	}
	if (!IsRegularFile(targetExe) || !IsRegularFile(runtimeDll))
	{
		std::wstring missing = L"";
		if (!IsRegularFile(targetExe))
			missing += L"  " + targetExe + L"\n";
		if (!IsRegularFile(runtimeDll))
			missing += L"  " + runtimeDll + L"\n";
		ShowMessage(L"缺少" + std::wstring(kTargetDescription) + L"程序:\n" + missing +
			L"\n请先在引擎仓库里构建它:\n  " + buildCommand +
			L"\n\n(引擎根: " + repoRoot + L")", MB_ICONERROR);
		return kExitMissingTarget;
	}

	// 4) 参数:显式 --project 打头,其余参数原样追加(方便以后扩展)。
	std::wstring arguments = L" --project \"" + projectRoot + L"\"";
	for (const std::wstring& extra : parsed.passthrough)
		arguments += L" " + QuoteArgument(extra);
	const std::wstring commandLine = L"\"" + targetExe + L"\"" + arguments;

	// 5) 拉起:工作目录 = 项目根;CreateProcess 被作业对象/策略拦下时退 ShellExecute。
	std::wstring environmentBlock = BuildEnvironmentBlock(projectRoot);
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	std::wstring writableCommandLine = commandLine;   // CreateProcessW 可能改写该缓冲
	const BOOL spawned = CreateProcessW(targetExe.c_str(), writableCommandLine.data(),
		nullptr, nullptr, FALSE, 0,
		environmentBlock.empty() ? nullptr : environmentBlock.data(),
		projectRoot.c_str(), &startup, &process);
	if (spawned)
	{
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return kExitOk;   // 启动器只负责拉起,不等目标进程退出。
	}

	const DWORD createProcessError = GetLastError();
	const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open", targetExe.c_str(),
		arguments.c_str(), projectRoot.c_str(), SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
	{
		ShowMessage(L"启动" + std::wstring(kTargetDescription) + L"失败:\n  " + targetExe +
			L"\n\nCreateProcess 错误码: " + std::to_wstring(static_cast<unsigned long>(createProcessError)) +
			L"\nShellExecute 错误码: " + std::to_wstring(reinterpret_cast<INT_PTR>(shellResult)) +
			L"\n\n项目: " + projectRoot, MB_ICONERROR);
		return kExitLaunchFailed;
	}
	return kExitOk;
}
