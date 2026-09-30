#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace World::Editor
{
	// 一个"正在运行的 Visual Studio 实例"的只读探测结果(COM Running Object Table)。
	struct RunningVisualStudio
	{
		// 该实例打开的**根**:Solution.FullName 是目录则原样,.sln/.slnx 则取父目录。
		std::wstring Root;
		// Solution.FullName 原文(诊断用)。
		std::wstring Solution;
		unsigned long Pid = 0;
		unsigned Version = 0;   // DTE 主版本(如 18 = VS 2026)
	};

	// 枚举正在运行的 VS 实例(只读)。整段 COM 探测在 worker 线程上做,最多等 timeoutMs;
	// 超时/失败返回空表(调用方回落"启动新实例"的既有路径),绝不抛异常、绝不阻塞 UI 线程。
	std::vector<RunningVisualStudio> EnumerateRunningVisualStudio(unsigned timeoutMs = 3000);

	// 纯匹配(无 COM):挑出 Root 覆盖 absFile 的实例,**最长的根优先**
	// (同时开着项目根与引擎根时,文件投给更具体的那一个)。
	bool FindRunningVisualStudioFor(const std::vector<RunningVisualStudio>& instances,
		const std::filesystem::path& absFile, RunningVisualStudio* out);

	// 在指定实例里打开文件并激活窗口(COM;同样受 timeoutMs 限制)。
	// 失败时 error 给可读原因;调用方应回落到既有启动路径。
	bool OpenFileInRunningVisualStudio(const RunningVisualStudio& instance,
		const std::filesystem::path& absFile, std::string* error, unsigned timeoutMs = 3000);
}
