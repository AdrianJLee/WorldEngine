#include "wldpch.h"
#include "Build/ProjectBuildRunner.h"

#include "World/Core/Log.h"

#include <deque>
#include <exception>
#include <utility>
#include <vector>

#ifdef WLD_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace World
{
	namespace
	{
		// 工作线程里保留的输出行数上限:只留出足够余量(比 kTailLines 大),交付时再裁到尾部 40 行。
		constexpr std::size_t kMaxKeptLines = 200;

		void AppendOutputLine(std::deque<std::string>& lines, std::string line)
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			lines.push_back(std::move(line));
			while (lines.size() > kMaxKeptLines)
				lines.pop_front();
		}

		std::string TailText(const std::deque<std::string>& lines)
		{
			const std::size_t begin = lines.size() > Editor::ProjectBuildRunner::kTailLines
				? lines.size() - Editor::ProjectBuildRunner::kTailLines : 0;
			std::string text;
			for (std::size_t index = begin; index < lines.size(); ++index)
			{
				if (!text.empty())
					text += '\n';
				text += lines[index];
			}
			return text;
		}
	}

	namespace Editor
	{
		ProjectBuildRunner::~ProjectBuildRunner()
		{
			Shutdown();
		}

#ifdef WLD_PLATFORM_WINDOWS
		namespace
		{
			std::wstring ToWide(const std::string& text)
			{
				if (text.empty())
					return std::wstring();
				const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
				if (size <= 0)
					return std::wstring();
				std::wstring wide(static_cast<std::size_t>(size), L'\0');
				MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
				wide.resize(static_cast<std::size_t>(size - 1));
				return wide;
			}

			std::wstring QuoteWide(const std::wstring& text)
			{
				return L"\"" + text + L"\"";
			}

			// HOTR-P3-T8:cmake 发现顺序 = PATH → VS 安装目录(与 templates/project-*/build.cmd
			// 的 find_vs_cmake 同一列表:Microsoft Visual Studio 下的 18/2025/2022 ×
			// Community/Professional/Enterprise/BuildTools)。
			std::filesystem::path FindCMakeExecutable()
			{
				std::vector<wchar_t> buffer(32768);
				const DWORD length = SearchPathW(nullptr, L"cmake.exe", nullptr,
					static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
				if (length > 0 && static_cast<std::size_t>(length) < buffer.size())
					return std::filesystem::path(std::wstring(buffer.data(), length));

				wchar_t programFiles[MAX_PATH] = {};
				if (GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH) == 0)
					return std::filesystem::path();
				const wchar_t* versions[] = { L"18", L"2025", L"2022" };
				const wchar_t* editions[] = { L"Community", L"Professional", L"Enterprise", L"BuildTools" };
				std::error_code candidateError;
				for (const wchar_t* version : versions)
				{
					for (const wchar_t* edition : editions)
					{
						const std::filesystem::path candidate = std::filesystem::path(programFiles)
							/ L"Microsoft Visual Studio" / version / edition
							/ L"Common7" / L"IDE" / L"CommonExtensions" / L"Microsoft"
							/ L"CMake" / L"CMake" / L"bin" / L"cmake.exe";
						if (std::filesystem::is_regular_file(candidate, candidateError))
							return candidate;
						candidateError.clear();
					}
				}
				return std::filesystem::path();
			}
		}
#endif

		bool ProjectBuildRunner::Start(const std::filesystem::path& projectRoot,
			const std::string& engineRoot, const std::string& configuration, std::string* error)
		{
			const auto fail = [error](const std::string& text)
			{
				if (error)
					*error = text;
				return false;
			};
			if (m_Running)
				return fail("a project build is already running");
			if (projectRoot.empty())
				return fail("no project root given for the build");
			if (engineRoot.empty())
				return fail("engine root (WLD_REPO_ROOT) is empty");
			const std::filesystem::path buildScript = projectRoot / "build.cmd";
			std::error_code existsError;
			if (!std::filesystem::is_regular_file(buildScript, existsError))
				return fail("no build.cmd in " + projectRoot.generic_string());

#ifdef WLD_PLATFORM_WINDOWS
			// /d 跳过 AutoRun;/s + 最外层引号 = cmd 的"带引号脚本 + 带引号参数"标准写法
			// (脚本路径与引擎根都可能含空格)。
			const std::wstring commandLine = L"/d /s /c \"\"" + buildScript.wstring() + L"\" \""
				+ std::filesystem::path(engineRoot).wstring() + L"\" " + ToWide(configuration) + L"\"";
			return StartCommand(commandLine, projectRoot, buildScript.generic_string(), error);
#else
			return fail("ProjectBuildRunner is only available in the Windows build (cmd.exe / build.cmd)");
#endif
		}

		// HOTR-P3-T8:插件"一键重载"的 CMake 目标构建(与 Start 共用同一套后台执行器)。
		bool ProjectBuildRunner::StartCMakeTarget(const std::filesystem::path& sourceDir,
			const std::filesystem::path& buildDir, const std::string& target,
			const std::string& configuration, const std::string& engineRoot, std::string* error)
		{
			const auto fail = [error](const std::string& text)
			{
				if (error)
					*error = text;
				return false;
			};
			if (m_Running)
				return fail("a project build is already running");
			if (sourceDir.empty())
				return fail("no CMake source directory given for the target build");
			if (buildDir.empty())
				return fail("no CMake build directory given for the target build");
			if (target.empty())
				return fail("no CMake target given for the target build");
			if (configuration.empty())
				return fail("no configuration given for the target build");
#ifdef WLD_PLATFORM_WINDOWS
			const std::filesystem::path cmake = FindCMakeExecutable();
			if (cmake.empty())
				return fail("cmake not found - neither on PATH nor in a Visual Studio install");

			const std::wstring cmakeQuoted = QuoteWide(cmake.wstring());
			std::wstring command = cmakeQuoted;
			std::error_code cacheError;
			if (std::filesystem::is_regular_file(buildDir / "CMakeCache.txt", cacheError))
			{
				// 已配置过:直接构建目标。
				command += L" --build " + QuoteWide(buildDir.wstring())
					+ L" --config " + ToWide(configuration)
					+ L" --target " + ToWide(target) + L" --parallel";
			}
			else
			{
				// 未配置过:先 configure(与 templates/project-*/build.cmd 同一生成器/平台),
				// 成功后接着构建同一个目标(cmd 的 && = configure 失败则不构建)。
				command += L" -S " + QuoteWide(sourceDir.wstring())
					+ L" -B " + QuoteWide(buildDir.wstring())
					+ L" -G \"Visual Studio 18 2026\" -A x64 -DCMAKE_BUILD_TYPE=" + ToWide(configuration);
				if (!engineRoot.empty())
					command += L" " + QuoteWide(L"-DWE_ROOT=" + std::filesystem::path(engineRoot).wstring());
				command += L" && " + cmakeQuoted + L" --build " + QuoteWide(buildDir.wstring())
					+ L" --config " + ToWide(configuration)
					+ L" --target " + ToWide(target) + L" --parallel";
			}
			return StartCommand(L"/d /s /c \"" + command + L"\"", sourceDir,
				"cmake target " + target, error);
#else
			return fail("ProjectBuildRunner is only available in the Windows build (cmake target builds)");
#endif
		}

		bool ProjectBuildRunner::StartCommand(const std::wstring& commandLine,
			const std::filesystem::path& workingDirectory, const std::string& description,
			std::string* error)
		{
			const auto fail = [error](const std::string& text)
			{
				if (error)
					*error = text;
				return false;
			};
			if (m_Running)
				return fail("a project build is already running");
			if (workingDirectory.empty())
				return fail("no working directory given for the build");

#ifdef WLD_PLATFORM_WINDOWS
			// 新一次构建从干净结果开始:buildOutput 不会把上一次的输出当成这一次的。
			m_HasResult = false;
			m_ExitCode = -1;
			m_OutputTail.clear();
			m_Outcome = Outcome {};
			m_Finished.store(false, std::memory_order_relaxed);

			SECURITY_ATTRIBUTES security {};
			security.nLength = sizeof(security);
			security.bInheritHandle = TRUE;

			HANDLE pipeRead = nullptr;
			HANDLE pipeWrite = nullptr;
			if (!CreatePipe(&pipeRead, &pipeWrite, &security, 0))
				return fail("cannot create the build output pipe (Win32 error "
					+ std::to_string(GetLastError()) + ")");
			// 读端只留给本进程:子进程退出(以及它的 cmake/MSBuild 子树退出)后立刻见 EOF。
			SetHandleInformation(pipeRead, HANDLE_FLAG_INHERIT, 0);

			// Job 对象:Shutdown 要能整棵树收掉(cmd.exe → cmake → MSBuild),不是只杀 cmd.exe。
			HANDLE job = CreateJobObjectW(nullptr, nullptr);
			if (job)
			{
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
				limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
			}

			wchar_t comspecBuffer[MAX_PATH] = {};
			const DWORD comspecLength = GetEnvironmentVariableW(L"COMSPEC", comspecBuffer, MAX_PATH);
			const std::wstring application = (comspecLength > 0 && comspecLength < MAX_PATH)
				? std::wstring(comspecBuffer) : std::wstring(L"cmd.exe");

			STARTUPINFOW startup {};
			startup.cb = sizeof(startup);
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
			startup.hStdOutput = pipeWrite;
			startup.hStdError = pipeWrite;

			PROCESS_INFORMATION process {};
			const std::wstring workingDirectoryText = workingDirectory.wstring();
			// CreateProcessW 允许原地改写 lpCommandLine ⇒ 必须传**可写**缓冲
			// (const std::wstring::data() 是 const wchar_t*，MSVC 直接拒绝)。
			std::wstring commandLineBuffer = commandLine;
			const BOOL created = CreateProcessW(application.c_str(), commandLineBuffer.data(),
				nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, workingDirectoryText.c_str(),
				&startup, &process);
			if (!created)
			{
				const DWORD code = GetLastError();
				CloseHandle(pipeRead);
				CloseHandle(pipeWrite);
				if (job)
					CloseHandle(job);
				return fail("cannot start '" + description
					+ "' (Win32 error " + std::to_string(code) + ")");
			}
			CloseHandle(pipeWrite);   // 父进程侧不需要写端(留着会让 EOF 永远不到)
			CloseHandle(process.hThread);
			if (job)
				AssignProcessToJobObject(job, process.hProcess);

			m_ProcessHandle = process.hProcess;
			m_JobHandle = job;   // Job 创建失败时为空:Shutdown 退化为只终止 cmd.exe
			m_PipeReadHandle = pipeRead;

			HANDLE processHandle = process.hProcess;
			try
			{
				m_Worker = std::thread([this, pipeRead, processHandle]
				{
					std::deque<std::string> lines;
					std::vector<char> buffer(4096);
					std::string pending;

					const auto splitPending = [&lines, &pending]()
					{
						std::size_t newline = pending.find('\n');
						while (newline != std::string::npos)
						{
							AppendOutputLine(lines, pending.substr(0, newline));
							pending.erase(0, newline + 1);
							newline = pending.find('\n');
						}
					};
					// 只读"已经到达"的字节,绝不阻塞在 ReadFile 上 —— 见下面的完成判据。
					const auto drainOutput = [&]()
					{
						DWORD available = 0;
						while (PeekNamedPipe(pipeRead, nullptr, 0, nullptr, &available, nullptr)
							&& available > 0)
						{
							DWORD read = 0;
							if (!ReadFile(pipeRead, buffer.data(), static_cast<DWORD>(buffer.size()),
									&read, nullptr) || read == 0)
								break;
							pending.append(buffer.data(), read);
							splitPending();
						}
					};

					// 完成判据 = **子进程退出**,不是管道 EOF:MSBuild 的常驻工作节点
					// (`/m` + 默认 node reuse)可能继承写端并活过构建,靠 EOF 会白等几分钟。
					// 退出后再排空一次:管道里剩下的字节在进程退出后仍可读。
					for (;;)
					{
						drainOutput();
						const DWORD waited = WaitForSingleObject(processHandle, 100);
						if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
							break;
					}
					drainOutput();
					if (!pending.empty())
						AppendOutputLine(lines, std::move(pending));

					DWORD exitCode = static_cast<DWORD>(-1);
					GetExitCodeProcess(processHandle, &exitCode);

					m_Outcome.ExitCode = static_cast<int>(exitCode);
					m_Outcome.Tail = TailText(lines);
					m_Finished.store(true, std::memory_order_release);
				});
			}
			catch (const std::exception& threadError)
			{
				// 线程没起来:把刚建的构建进程树立刻收掉(不留"没有读取者的孤儿构建")。
				if (job)
					TerminateJobObject(job, 1);
				else
					TerminateProcess(process.hProcess, 1);
				CloseChildHandles();
				return fail(std::string("cannot start the build worker thread: ") + threadError.what());
			}
			m_Running = true;
			return true;
#else
			return fail("ProjectBuildRunner is only available in the Windows build");
#endif
		}

		void ProjectBuildRunner::Poll()
		{
			if (!m_Running || !m_Finished.load(std::memory_order_acquire))
				return;
			JoinWorker();
			m_Running = false;
			m_ExitCode = m_Outcome.ExitCode;
			m_OutputTail = m_Outcome.Tail;
			m_HasResult = true;
			CloseChildHandles();
		}

		void ProjectBuildRunner::Shutdown()
		{
			if (!m_Running)
			{
				JoinWorker();   // 防御:上一次 join 失败/未走到时补一次(幂等)
				CloseChildHandles();
				return;
			}
#ifdef WLD_PLATFORM_WINDOWS
			// 终止整棵构建进程树再 join:关编辑器不能等一次构建(分钟级)跑完。
			if (m_JobHandle)
			{
				TerminateJobObject(static_cast<HANDLE>(m_JobHandle), 1);
				WLD_CORE_WARN("[cppbuild] cancelled the in-flight project build (editor is shutting down)");
			}
			else if (m_ProcessHandle)
			{
				TerminateProcess(static_cast<HANDLE>(m_ProcessHandle), 1);
				WLD_CORE_WARN("[cppbuild] cancelled the in-flight project build (no job object; cmd.exe only)");
			}
#endif
			JoinWorker();
			m_Running = false;
			m_HasResult = false;
			m_ExitCode = -1;
			CloseChildHandles();
		}

		void ProjectBuildRunner::JoinWorker()
		{
			if (m_Worker.joinable())
				m_Worker.join();
		}

		void ProjectBuildRunner::CloseChildHandles()
		{
#ifdef WLD_PLATFORM_WINDOWS
			if (m_PipeReadHandle)
			{
				CloseHandle(static_cast<HANDLE>(m_PipeReadHandle));
				m_PipeReadHandle = nullptr;
			}
			if (m_ProcessHandle)
			{
				CloseHandle(static_cast<HANDLE>(m_ProcessHandle));
				m_ProcessHandle = nullptr;
			}
			if (m_JobHandle)
			{
				CloseHandle(static_cast<HANDLE>(m_JobHandle));
				m_JobHandle = nullptr;
			}
#else
			m_PipeReadHandle = nullptr;
			m_ProcessHandle = nullptr;
			m_JobHandle = nullptr;
#endif
		}
	}
}
