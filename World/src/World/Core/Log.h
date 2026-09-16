#pragma once
#include "World/Core/Core.h"
#include "World/Core/Export.h"

#include "spdlog/spdlog.h"
#include "spdlog/fmt/ostr.h"

#include <cstddef>
#include <string>
#include <vector>

namespace World
{
	class  Log
	{
	public:
		static void Init();

		inline static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
		inline static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }
		// 最近的日志行(环形缓冲,已格式化)。给 AI 控制通道的 log.tail 用:
		// 没有它就只能靠 stdout 重定向,拿不到"运行时刚刚发生了什么"。
		static std::vector<std::string> RecentLines(size_t maxLines);
	private:
		static WLD_API std::shared_ptr<spdlog::logger> s_CoreLogger;
		static WLD_API std::shared_ptr<spdlog::logger> s_ClientLogger;
	};
}

// Core log macros。日志器未初始化(无头测试/静态析构期)时静默跳过:
// 之前的裸 ->info() 在 logger 为空指针时会直接崩溃。
#define WLD_CORE_TRACE(...) do { if (auto logger = ::World::Log::GetCoreLogger()) logger->trace(__VA_ARGS__); } while (0)
#define WLD_CORE_INFO(...) do { if (auto logger = ::World::Log::GetCoreLogger()) logger->info(__VA_ARGS__); } while (0)
#define WLD_CORE_WARN(...) do { if (auto logger = ::World::Log::GetCoreLogger()) logger->warn(__VA_ARGS__); } while (0)
#define WLD_CORE_ERROR(...) do { if (auto logger = ::World::Log::GetCoreLogger()) logger->error(__VA_ARGS__); } while (0)
#define WLD_CORE_CRITICAL(...) do { if (auto logger = ::World::Log::GetCoreLogger()) logger->critical(__VA_ARGS__); } while (0)

// Client log macros
#define WLD_TRACE(...) do { if (auto logger = ::World::Log::GetClientLogger()) logger->trace(__VA_ARGS__); } while (0)
#define WLD_INFO(...) do { if (auto logger = ::World::Log::GetClientLogger()) logger->info(__VA_ARGS__); } while (0)
#define WLD_WARN(...) do { if (auto logger = ::World::Log::GetClientLogger()) logger->warn(__VA_ARGS__); } while (0)
#define WLD_ERROR(...) do { if (auto logger = ::World::Log::GetClientLogger()) logger->error(__VA_ARGS__); } while (0)
#define WLD_CRITICAL(...) do { if (auto logger = ::World::Log::GetClientLogger()) logger->critical(__VA_ARGS__); } while (0)
