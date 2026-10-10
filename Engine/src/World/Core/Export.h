#pragma once

// World 目标构建为 WorldRuntime.dll 时定义为 dllexport，消费方为 dllimport。
// 用于跨 DLL 共享与导出的公共符号（系统服务、核心契约类与数据模型）。
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
