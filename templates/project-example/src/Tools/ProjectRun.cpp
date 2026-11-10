// PROJ-15/T1:项目自己的可执行启动目标 —— ProjectRun.exe(默认开编辑器,`--play` 跑游戏)。
//
// 目的:VS 的 CMake 工程模式只在 Startup Item 下拉里列**可执行 CMake 目标**(官方文档口径),
// 而项目侧 World 是库、Game 是 DLL、Editor/Runtime 属引擎侧 —— 于是给项目补一个自己的
// 可执行目标,并把"用哪个引擎、项目根在哪、开编辑器还是跑游戏"收进这一份源码:
//
//   ProjectRun.exe        ⇒ <引擎根>/build/x64-<cfg>/Editor/<cfg>/Editor.exe   --project <项目根>
//   ProjectRun.exe --play ⇒ <引擎根>/build/x64-<cfg>/Runtime/<cfg>/Runtime.exe --project <项目根>
//
// 默认就是"打开编辑器":VS 的启动项下拉里只有这一项,不需要用户在两项之间挑。`--play`
// 由本启动器**消费**,不转交给子进程;其余参数一律原样透传。
//
// 硬性约束:
//  * 零引擎依赖:不链接 World/Game,进程不加载 WorldRuntime.dll ⇒ 只依赖系统 DLL。
//  * 项目根 = exe 自身所在目录(GetModuleFileNameW);`--project <dir>` 可覆盖。
//  * 引擎根解析顺序与 <项目根>/build.cmd 同源:
//      1) 参数 --engine <dir>
//      2) 环境变量 WE_ROOT
//      3) <项目根>/.we/engine-root.txt(一行 UTF-8,向导写入;口径见 build.cmd 的 :read_engine_root_file)
//      4) 相对布局 <项目根>\..\(模板位于 <引擎根>\templates\project-<id>\ 时的口径)
//    1)/2) 是显式值:指向的不是引擎 checkout 就直接报错,不静默换别的来源(与 build.cmd 一致)。
//  * 工作目录 = 项目根;CreateProcessW 起不来时退 ShellExecuteW;拉起成功后**等子进程退出**,
//    并把它的退出码当作自己的退出码(ShellExecute 路径拿不到子进程句柄,只能返回 0)。
//  * GUI 子系统:双击不弹控制台;所有失败(引擎根/目标程序缺失、拉起失败)一律
//    MessageBoxW 给出可读原因 + 非 0 退出码,不静默。
//
// 只有一个可执行目标(见旁路 CMakeLists.txt):下拉里因此只有一项,且默认已选中
// "Open Editor (this project)";想直接跑游戏就加 `--play`,或双击项目根的
// `<项目名>-Play.exe`(那是引擎自带的启动器 exe,与这里无关)。
//
// 备注:项目模板的 src/** 会被引擎侧 Game/CMakeLists.txt 的通配一并收进 Game.dll(那条通配
// 不在本任务的修改边界内),所以"编译 Game 模块"的那次编译(GAME_BUILD_DLL)整份跳过 ——
// 启动器不属于 Game.dll。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(GAME_BUILD_DLL)

// Game.dll 的 src/** 通配看到本文件时的编译:整份跳过。留一个符号,避免"空编译单元"告警。
namespace
{
	[[maybe_unused]] const int kProjectRunIsNotPartOfGameModule = 0;
}

#else

#if !defined(WLD_BUILD_TYPE) || !defined(WLD_OUTPUT_DIR)
#error "ProjectRun.cpp:缺少 WLD_BUILD_TYPE / WLD_OUTPUT_DIR(见旁路 CMakeLists.txt)"
#endif

namespace
{
	constexpr wchar_t kRunnerName[] = L"ProjectRun.exe";

	// 默认宿主 = 编辑器;`--play` 换成运行时。
	struct HostTarget
	{
		const wchar_t* Folder;        // 引擎构建目录里的模块子目录
		const wchar_t* ExeName;       // 宿主可执行文件名
		const wchar_t* CMakeName;     // 缺产物时要构建的 CMake 目标
		const wchar_t* Description;   // 弹窗里的中文名
	};

	constexpr HostTarget kEditorHost{ L"Editor", L"Editor.exe", L"Editor", L"编辑器" };
	constexpr HostTarget kRuntimeHost{ L"Runtime", L"Runtime.exe", L"Runtime", L"运行时" };

	// 退出码(自动化探针按码判断,不要只看弹窗文本):
	//   0 = 子进程正常退出(或 ShellExecute 拉起成功);
	//   1 = 命令行/项目目录有问题;2 = 引擎根找不到;3 = 目标程序缺失;4 = 拉起失败;
	//   其余 = 子进程自己的退出码(原样回传)。
	constexpr int kExitOk = 0;
	constexpr int kExitBadArguments = 1;
	constexpr int kExitEngineRootMissing = 2;
	constexpr int kExitTargetMissing = 3;
	constexpr int kExitLaunchFailed = 4;

	void ShowMessage(const std::wstring& text, UINT icon)
	{
		// MB_SETFOREGROUND:从 VS / 资源管理器拉起时保证弹窗在前台,不会被别的窗口盖住。
		MessageBoxW(nullptr, text.c_str(), L"WorldEngine 项目启动器", MB_OK | MB_SETFOREGROUND | icon);
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

	// 归一化成 Windows 路径:正斜杠(CMake 的写法)→ 反斜杠;去掉尾分隔符。
	// 去掉尾分隔符是硬要求 —— `--project "D:\proj\"` 里的 `\"` 会被 CRT 解析成字面引号。
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

	// 把可能带 `..` 的路径折成绝对路径(相对布局兜底要用);失败时保持原样。
	std::wstring CanonicalPath(const std::wstring& path)
	{
		const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
		if (needed == 0)
			return path;
		std::wstring buffer(static_cast<size_t>(needed), L'\0');
		const DWORD written = GetFullPathNameW(path.c_str(), needed, buffer.data(), nullptr);
		if (written == 0 || written >= needed)
			return path;
		buffer.resize(written);
		return NormalizePath(std::move(buffer));
	}

	// exe 自身所在目录 = 默认项目根。
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

	std::wstring EnvironmentVariable(const wchar_t* name)
	{
		for (DWORD capacity = MAX_PATH; capacity <= 32768; capacity *= 2)
		{
			std::wstring buffer(capacity, L'\0');
			const DWORD length = GetEnvironmentVariableW(name, buffer.data(), capacity);
			if (length == 0)
				return {};   // 未定义或空值都按"没给"处理。
			if (length < capacity)
			{
				buffer.resize(length);
				return buffer;
			}
		}
		return {};
	}

	struct ParsedCommandLine
	{
		std::wstring project;                    // --project 显式指定(空 = 用 exe 所在目录)
		std::wstring engine;                     // --engine 显式指定(空 = 按解析顺序兜底)
		bool play = false;                       // --play:拉起 Runtime 而不是 Editor
		std::vector<std::wstring> passthrough;   // 其余参数原样转交目标 exe
		bool projectFlagMissingValue = false;    // 只写了 --project 但后面没有目录
		bool engineFlagMissingValue = false;     // 只写了 --engine 但后面没有目录
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
			// --play 由本启动器消费(决定拉起 Runtime 还是 Editor),不转交子进程。
			if (EqualsIgnoreCase(argument, L"--play"))
			{
				parsed.play = true;
				continue;
			}
			const bool isProjectFlag = EqualsIgnoreCase(argument, L"--project");
			const bool isEngineFlag = EqualsIgnoreCase(argument, L"--engine");
			if (isProjectFlag || isEngineFlag)
			{
				if (index + 1 >= argumentCount || arguments[index + 1][0] == L'\0')
				{
					if (isProjectFlag)
						parsed.projectFlagMissingValue = true;
					else
						parsed.engineFlagMissingValue = true;
				}
				else if (isProjectFlag)
					parsed.project = arguments[++index];
				else
					parsed.engine = arguments[++index];
				continue;
			}

			const bool isPrefixedProject = StartsWithIgnoreCase(argument, L"--project=");
			const bool isPrefixedEngine = StartsWithIgnoreCase(argument, L"--engine=");
			if (isPrefixedProject || isPrefixedEngine)
			{
				const std::wstring value = argument.substr(std::wcslen(isPrefixedProject ? L"--project=" : L"--engine="));
				if (value.empty())
				{
					if (isPrefixedProject)
						parsed.projectFlagMissingValue = true;
					else
						parsed.engineFlagMissingValue = true;
				}
				else if (isPrefixedProject)
					parsed.project = value;
				else
					parsed.engine = value;
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

	// 引擎 checkout 的判据(比 build.cmd 的"只看 CMakeLists.txt"再严一点,避免把别的
	// CMake 工程当成引擎根;模板 CMakeLists.txt 里的相对布局兜底也是这个口径)。
	bool LooksLikeEngineRoot(const std::wstring& path)
	{
		return IsRegularFile(JoinPath(path, L"CMakeLists.txt"))
			&& IsRegularFile(JoinPath(JoinPath(path, L"Engine"), L"CMakeLists.txt"))
			&& IsRegularFile(JoinPath(JoinPath(path, L"Game"), L"CMakeLists.txt"));
	}

	// <项目根>/.we/engine-root.txt:一行 UTF-8(无 BOM;向导 ProjectScaffolder 的口径)。
	// 容忍 BOM 与行尾 CR/LF,取第一行。
	std::wstring ReadEngineRootFile(const std::wstring& projectRoot)
	{
		std::ifstream stream(JoinPath(JoinPath(projectRoot, L".we"), L"engine-root.txt"), std::ios::binary);
		if (!stream)
			return {};
		std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		if (bytes.size() >= 3
			&& static_cast<unsigned char>(bytes[0]) == 0xEF
			&& static_cast<unsigned char>(bytes[1]) == 0xBB
			&& static_cast<unsigned char>(bytes[2]) == 0xBF)
			bytes.erase(0, 3);
		if (const size_t newline = bytes.find_first_of("\r\n"); newline != std::string::npos)
			bytes.resize(newline);
		return CanonicalPath(NormalizePath(WideFromUtf8(bytes.c_str())));
	}
}

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previousInstance*/,
	LPWSTR /*commandLine*/, int /*showCommand*/)
{
	const ParsedCommandLine parsed = ParseCommandLine();
	if (parsed.projectFlagMissingValue)
	{
		ShowMessage(std::wstring(L"--project 后面需要一个项目目录,例如:\n\n  ") + kRunnerName +
			L" --project \"D:\\MyGame\"", MB_ICONERROR);
		return kExitBadArguments;
	}
	if (parsed.engineFlagMissingValue)
	{
		ShowMessage(std::wstring(L"--engine 后面需要一个引擎 checkout 目录,例如:\n\n  ") + kRunnerName +
			L" --engine \"E:\\WorldEngine\"", MB_ICONERROR);
		return kExitBadArguments;
	}

	// 1) 项目根:--project 显式优先,否则 = exe 自身所在目录。
	std::wstring projectRoot = NormalizePath(parsed.project);
	if (projectRoot.empty())
		projectRoot = NormalizePath(ModuleDirectory());
	if (projectRoot.empty() || !IsDirectory(projectRoot))
	{
		ShowMessage(std::wstring(L"项目目录不存在或无法确定:\n  ") +
			(projectRoot.empty() ? std::wstring(L"(exe 自身所在目录)") : projectRoot) +
			L"\n\n把 " + kRunnerName + L" 放进项目根目录运行,或用 --project <目录> 显式指定。",
			MB_ICONERROR);
		return kExitBadArguments;
	}

	// 2) 引擎根:顺序与 <项目根>/build.cmd 同源。1)/2) 是显式来源,给出的是坏路径就直接报错。
	std::wstring engineRoot;
	std::wstring engineRootSource;
	const std::wstring explicitEngine = NormalizePath(parsed.engine);
	const std::wstring environmentRoot = NormalizePath(EnvironmentVariable(L"WE_ROOT"));
	const std::wstring recordedRoot = ReadEngineRootFile(projectRoot);
	if (!explicitEngine.empty())
	{
		if (!LooksLikeEngineRoot(explicitEngine))
		{
			ShowMessage(L"--engine 指向的不是 WorldEngine checkout:\n  " + explicitEngine +
				L"\n\n需要目录里有 CMakeLists.txt、Engine\\CMakeLists.txt 与 Game\\CMakeLists.txt。",
				MB_ICONERROR);
			return kExitEngineRootMissing;
		}
		engineRoot = explicitEngine;
		engineRootSource = L"--engine";
	}
	else if (!environmentRoot.empty())
	{
		if (!LooksLikeEngineRoot(environmentRoot))
		{
			ShowMessage(L"环境变量 WE_ROOT 指向的不是 WorldEngine checkout:\n  " + environmentRoot +
				L"\n\n需要目录里有 CMakeLists.txt、Engine\\CMakeLists.txt 与 Game\\CMakeLists.txt。",
				MB_ICONERROR);
			return kExitEngineRootMissing;
		}
		engineRoot = environmentRoot;
		engineRootSource = L"环境变量 WE_ROOT";
	}
	else if (!recordedRoot.empty())
	{
		if (LooksLikeEngineRoot(recordedRoot))
		{
			engineRoot = recordedRoot;
			engineRootSource = L".we\\engine-root.txt";
		}
	}
	if (engineRoot.empty())
	{
		const std::wstring relativeRoot = CanonicalPath(JoinPath(JoinPath(projectRoot, L".."), L".."));
		if (LooksLikeEngineRoot(relativeRoot))
		{
			engineRoot = relativeRoot;
			engineRootSource = L"相对布局 ..\\..";
		}
	}
	if (engineRoot.empty())
	{
		std::wstring message = L"找不到 WorldEngine 引擎根(";
		message += kRunnerName;
		message += L")。\n\n尝试顺序:\n";
		message += L"  1) --engine <目录>\n  2) 环境变量 WE_ROOT\n";
		message += L"  3) <项目根>\\.we\\engine-root.txt\n  4) 相对布局 ..\\..\n\n";
		message += L"项目根:\n  " + projectRoot + L"\n\n";
		message += L"请把引擎 checkout 的路径写进 .we\\engine-root.txt,或用 --engine 指定。";
		ShowMessage(message, MB_ICONERROR);
		return kExitEngineRootMissing;
	}

	// 3) 目标宿主:默认编辑器,--play 换成运行时(见文件头)。
	const HostTarget& host = parsed.play ? kRuntimeHost : kEditorHost;
	// <引擎根>/<WLD_OUTPUT_DIR><模块>/<WLD_BUILD_TYPE>/<目标>.exe(编译期宏带尾分隔符)。
	const std::wstring outputDirectory = NormalizePath(WideFromUtf8(WLD_OUTPUT_DIR));
	const std::wstring buildType = NormalizePath(WideFromUtf8(WLD_BUILD_TYPE));
	const std::wstring targetDirectory =
		JoinPath(JoinPath(JoinPath(engineRoot, outputDirectory), host.Folder), buildType);
	const std::wstring targetExe = JoinPath(targetDirectory, host.ExeName);
	const std::wstring runtimeDll = JoinPath(targetDirectory, L"WorldRuntime.dll");

	if (!IsRegularFile(targetExe) || !IsRegularFile(runtimeDll))
	{
		std::wstring missing;
		if (!IsRegularFile(targetExe))
			missing += L"  " + targetExe + L"\n";
		if (!IsRegularFile(runtimeDll))
			missing += L"  " + runtimeDll + L"\n";
		ShowMessage(L"缺少" + std::wstring(host.Description) + L"程序:\n" + missing +
			L"\n请在引擎仓库里构建它(工作目录 = 引擎根):\n  cmake --build " + outputDirectory +
			L" --config " + buildType + L" --target " + host.CMakeName +
			L"\n\n引擎根: " + engineRoot + L"(来源: " + engineRootSource +
			L")\n启动模式: " + (parsed.play ? L"--play(运行时)" : L"默认(编辑器)"), MB_ICONERROR);
		return kExitTargetMissing;
	}

	// 4) 参数:显式 --project 打头,其余参数原样追加(方便以后扩展 / VS 启动项)。
	std::wstring arguments = L"--project \"" + projectRoot + L"\"";
	for (const std::wstring& extra : parsed.passthrough)
		arguments += L" " + QuoteArgument(extra);
	const std::wstring commandLine = L"\"" + targetExe + L"\" " + arguments;

	// 5) 拉起:工作目录 = 项目根;CreateProcess 被作业对象/策略拦下时退 ShellExecute。
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	std::wstring writableCommandLine = commandLine;   // CreateProcessW 可能改写该缓冲
	const BOOL spawned = CreateProcessW(targetExe.c_str(), writableCommandLine.data(),
		nullptr, nullptr, FALSE, 0, nullptr, projectRoot.c_str(), &startup, &process);
	if (spawned)
	{
		CloseHandle(process.hThread);
		const DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
		DWORD childExitCode = 0;
		const BOOL read = GetExitCodeProcess(process.hProcess, &childExitCode);
		CloseHandle(process.hProcess);
		if (waited != WAIT_OBJECT_0 || !read)
			return kExitLaunchFailed;
		return static_cast<int>(childExitCode);   // 子进程的退出码原样回传。
	}

	const DWORD createProcessError = GetLastError();
	const HINSTANCE shellResult = ShellExecuteW(nullptr, L"open", targetExe.c_str(),
		arguments.c_str(), projectRoot.c_str(), SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(shellResult) <= 32)
	{
		ShowMessage(L"启动" + std::wstring(host.Description) + L"失败:\n  " + targetExe +
			L"\n\nCreateProcess 错误码: " + std::to_wstring(static_cast<unsigned long>(createProcessError)) +
			L"\nShellExecute 错误码: " + std::to_wstring(reinterpret_cast<INT_PTR>(shellResult)) +
			L"\n\n项目: " + projectRoot, MB_ICONERROR);
		return kExitLaunchFailed;
	}
	return kExitOk;   // ShellExecute 没有子进程句柄,拿不到退出码。
}

#endif   // GAME_BUILD_DLL
