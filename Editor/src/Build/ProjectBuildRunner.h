#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>

namespace World::Editor
{
	// HOTR-P3-T7(P3-a):编辑器内"构建项目 C++ 模块"的后台执行器。
	//
	// 动机:现有的两段式热重载(File ▸ Reload C++ Module / AI `module.reload`)只负责
	// 卸载 + 加载 `Game.dll`;"构建"这一步要用户自己离开编辑器去 VS/CMake 里跑。本执行器
	// 把项目自带的构建脚本(`<项目根>/build.cmd`)放到工作线程跑:
	//   编辑者(EditorLayer)在启动前先卸载 Game 模块(Windows 上 Editor 映射着 DLL 时
	//   链接器无法改写该文件),构建成功后走加载段 ⇒ 一次点击 = 卸载 + 构建 + 加载新产物。
	//
	// 线程纪律(与编辑器既有后台服务同一口径):
		//  - Start / Poll / Shutdown 只在**主线程**调用;Start 内同步建进程(失败原因立刻可读),
		//    读取子进程输出与等待退出在工作线程;
		//  - 子进程 stdout+stderr 合并读取,只保留尾部 `kTailLines` 行(滚动丢弃更早的行);
		//    完成判据 = 子进程退出(不靠管道 EOF:MSBuild 的常驻工作节点可能继承写端);
	//  - 结果(退出码 + 输出尾部)由工作线程经 m_Finished 发布(acquire/release 配对),
	//    只有 Poll 之后才可查询 —— Poll 未调用前 IsRunning() 一直为 true;
	//  - 同一时刻只允许一个构建;Start 在飞时返回可读拒绝;
	//  - Shutdown 终止在飞构建(Job 对象 = 整棵进程树:cmd.exe → cmake → MSBuild)再 join,
	//    避免关编辑器被一次构建阻塞数分钟。
	//
	// 平台:构建入口是 Windows 批处理(`cmd.exe /c build.cmd …`),按 Windows 原生目标实现;
	// 非 Windows 构建里 Start 返回可读错误(编辑器宿主本身也只在 Windows 构建)。
	class ProjectBuildRunner
	{
	public:
		ProjectBuildRunner() = default;
		~ProjectBuildRunner();

		ProjectBuildRunner(const ProjectBuildRunner&) = delete;
		ProjectBuildRunner& operator=(const ProjectBuildRunner&) = delete;

		// 主线程:启动一次 `cmd.exe /d /s /c ""<projectRoot>\build.cmd" "<engineRoot>" <configuration>"`
		// (cwd = projectRoot;stdout+stderr 合并重定向到管道,不弹控制台窗口)。
		// 失败(已在构建 / 参数为空 / build.cmd 不存在 / 进程创建失败)= false + 可读 error。
		bool Start(const std::filesystem::path& projectRoot, const std::string& engineRoot,
			const std::string& configuration, std::string* error = nullptr);

		// HOTR-P3-T8:在同一后台执行器上构建一个 CMake 目标(插件"一键重载"用)。
		//
		//   * buildDir/CMakeCache.txt 存在 → 直接
		//     `cmake --build <buildDir> --config <cfg> --target <target> --parallel`;
		//   * 不存在 → 先
		//     `cmake -S <sourceDir> -B <buildDir> -G "Visual Studio 18 2026" -A x64
		//      -DCMAKE_BUILD_TYPE=<cfg> [-DWE_ROOT=<engineRoot>]`,再构建同一个目标。
		//
		// cmake 发现顺序 = PATH → VS 安装目录(与 templates/project-*/build.cmd 的
		// find_vs_cmake 同一列表:Microsoft Visual Studio 下的 18/2025/2022 ×
		// Community/Professional/Enterprise/BuildTools)。工作目录 = sourceDir。
		// 契约与 Start() 相同:主线程调用;同一时刻只允许一个构建(在飞 = 可读拒绝);
		// 结果同样经 Poll() 发布(ExitCode/OutputTail;configure 与 build 的输出合并保留尾部)。
		bool StartCMakeTarget(const std::filesystem::path& sourceDir,
			const std::filesystem::path& buildDir, const std::string& target,
			const std::string& configuration, const std::string& engineRoot,
			std::string* error = nullptr);

		// 主线程帧边界:构建结束后 join 工作线程,把退出码与输出尾部变成可查询结果。
		// 没有在飞构建 / 已消费过 = no-op(幂等)。
		void Poll();

		// 构建在飞(含"进程已退出但还没 Poll"这一段,保证轮询方至少能看到一次 true)。
		bool IsRunning() const { return m_Running; }
		// Poll 完成过至少一次结果(下一个 Start 会清掉)。
		bool HasResult() const { return m_HasResult; }
		// 最近一次结果的进程退出码;被 Shutdown 终止或没有结果时为 -1。
		int ExitCode() const { return m_ExitCode; }
		// 最近一次结果的输出尾部(stdout+stderr 合并;每个 Start 清空,最多 kTailLines 行)。
		const std::string& OutputTail() const { return m_OutputTail; }

		// 主线程:终止在飞构建并 join(幂等;OnDetach 与析构都会调)。
		void Shutdown();

		// 输出尾部保留的行数(派工单口径 = 最近 ~40 行)。
		static constexpr std::size_t kTailLines = 40;

	private:
		// 结果只在 m_Finished(acquire)之后被主线程读取(与 EditorLayer 的烹饪线程同一口径)。
		struct Outcome
		{
			int ExitCode = -1;
			std::string Tail;
		};

		void JoinWorker();
		void CloseChildHandles();
		// 共用的 Windows 启动段(Start / StartCMakeTarget 都从这里进):建管道 + Job 对象 +
		// cmd.exe /d /s /c <commandLine> + 输出读取线程。description 只用于可读错误。
		bool StartCommand(const std::wstring& commandLine, const std::filesystem::path& workingDirectory,
			const std::string& description, std::string* error);

		std::thread m_Worker;
		std::atomic<bool> m_Finished { false };
		Outcome m_Outcome;

		// 子进程句柄(Windows HANDLE 原样保存,避免头文件引入 windows.h;仅主线程读写)。
		void* m_ProcessHandle = nullptr;
		void* m_JobHandle = nullptr;
		void* m_PipeReadHandle = nullptr;

		// 只由主线程读写(Start / Poll / Shutdown 都在主线程)。
		bool m_Running = false;
		bool m_HasResult = false;
		int m_ExitCode = -1;
		std::string m_OutputTail;
	};
}
