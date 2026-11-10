// 调用点(Site)符号化 + 转储。
//
// 为什么单独一个文件:符号化依赖平台调试库(DbgHelp),而采集路径必须零依赖、零分配。
// 两者混在一起会让"采集"被"解析"拖累(旧 Instrumentor 的教训:观测手段本身必须便宜)。
//
// 三条纪律:
//   1. **只在转储时解析**(转储是低频诊断,不在帧循环里);
//   2. 解析结果**缓存**(同一地址不重复查符号表);
//   3. 符号化自身会分配 —— 用抑制标记把这些分配排除在归因之外,
//      否则"观察者"会污染"被观察对象"(这正是我们在修的老毛病)。

#include "wldpch.h"
#include "World/Profiling/SymbolResolver.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#if defined(WLD_PLATFORM_WINDOWS)
#include <Windows.h>
#include <dbghelp.h>
#endif

namespace World::Profiling
{
	namespace
	{
		std::atomic<bool> g_Suppress { false };

		// 地址 → 解析结果。整个进程一份;命中率极高(调用点数量有限)。
		std::mutex g_CacheMutex;
		std::unordered_map<uintptr_t, std::string> g_Cache;
		bool g_DbgHelpReady = false;
		bool g_DbgHelpFailed = false;

		bool EnsureDbgHelp()
		{
#if defined(WLD_PLATFORM_WINDOWS)
			if (g_DbgHelpReady)
				return true;
			if (g_DbgHelpFailed)
				return false;
			const HANDLE process = GetCurrentProcess();
			SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
			if (!SymInitialize(process, nullptr, TRUE))
			{
				g_DbgHelpFailed = true;
				return false;
			}
			g_DbgHelpReady = true;
			return true;
#else
			return false;
#endif
		}
	}

	bool SymbolResolver::Suppressed() noexcept
	{
		return g_Suppress.load(std::memory_order_relaxed);
	}

	SymbolResolver::SuppressionScope::SuppressionScope() noexcept
	{
		g_Suppress.store(true, std::memory_order_relaxed);
	}

	SymbolResolver::SuppressionScope::~SuppressionScope()
	{
		g_Suppress.store(false, std::memory_order_relaxed);
	}

	std::string SymbolResolver::Describe(void* address)
	{
		if (!address)
			return "<null>";

		const uintptr_t key = reinterpret_cast<uintptr_t>(address);
		{
			std::lock_guard<std::mutex> guard(g_CacheMutex);
			const auto it = g_Cache.find(key);
			if (it != g_Cache.end())
				return it->second;
		}

		char text[512];
		std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(key));
		std::string result = text;

#if defined(WLD_PLATFORM_WINDOWS)
		if (EnsureDbgHelp())
		{
			const HANDLE process = GetCurrentProcess();
			const DWORD64 base = SymGetModuleBase64(process, key);
			char moduleName[MAX_PATH] = {};
			if (base != 0)
				GetModuleFileNameA(reinterpret_cast<HMODULE>(base), moduleName, MAX_PATH);

			alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256] = {};
			auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 255;

			DWORD64 displacement = 0;
			char lineText[64] = {};
			if (SymFromAddr(process, key, &displacement, symbol))
			{
				DWORD lineDisplacement = 0;
				IMAGEHLP_LINE64 line {};
				line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
				if (SymGetLineFromAddr64(process, key, &lineDisplacement, &line))
					std::snprintf(lineText, sizeof(lineText), " [%s:%lu]",
						line.FileName ? line.FileName : "?", line.LineNumber);
				result = std::string(moduleName) + "!" + symbol->Name + "+" +
					std::to_string(static_cast<unsigned long long>(displacement)) + lineText;
			}
			else
			{
				// 没符号也别丢信息:至少给出模块+偏移(可用 dumpbin/PIX 继续查)。
				std::snprintf(text, sizeof(text), "0x%llX (%s+0x%llX)",
					static_cast<unsigned long long>(key),
					moduleName[0] ? moduleName : "?",
					static_cast<unsigned long long>(base ? key - base : 0));
				result = text;
			}
		}
#endif

		{
			std::lock_guard<std::mutex> guard(g_CacheMutex);
			g_Cache.emplace(key, result);
		}
		return result;
	}
}
