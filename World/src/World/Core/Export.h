#pragma once

// World 目标构建为 WorldRuntime.dll 时定义为 dllexport，消费方为 dllimport。
// 仅用于跨 DLL 共享的符号（单例状态、静态数据成员）；普通符号由
// WINDOWS_EXPORT_ALL_SYMBOLS 兜底导出。
#if defined(WLD_PLATFORM_WINDOWS)
	#if defined(WLD_BUILD_DLL)
		#define WLD_API __declspec(dllexport)
	#else
		#define WLD_API __declspec(dllimport)
	#endif
#else
	#if defined(WLD_BUILD_DLL)
		#define WLD_API __attribute__((visibility("default")))
	#else
		#define WLD_API
	#endif
#endif
