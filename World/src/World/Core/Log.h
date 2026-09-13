#pragma once
#include "World/Core/Core.h"
#include "World/Core/Export.h"

#include "spdlog/spdlog.h"
#include "spdlog/fmt/ostr.h"

namespace World
{
	class  Log
	{
	public:
		static void Init();

		inline static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }
		inline static std::shared_ptr<spdlog::logger>& GetClientLogger() { return s_ClientLogger; }
	private:
		static WLD_API std::shared_ptr<spdlog::logger> s_CoreLogger;
		static WLD_API std::shared_ptr<spdlog::logger> s_ClientLogger;
	};
}

//Core log macros
#define WLD_CORE_TRACE(...) ::World::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define WLD_CORE_INFO(...) ::World::Log::GetCoreLogger()->info(__VA_ARGS__)
#define WLD_CORE_WARN(...) ::World::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define WLD_CORE_ERROR(...) ::World::Log::GetCoreLogger()->error(__VA_ARGS__)
#define WLD_CORE_CRITICAL(...) ::World::Log::GetCoreLogger()->critical(__VA_ARGS__)

//Client log macros
#define WLD_TRACE(...) ::World::Log::GetClientLogger()->trace(__VA_ARGS__)
#define WLD_INFO(...) ::World::Log::GetClientLogger()->info(__VA_ARGS__)
#define WLD_WARN(...) ::World::Log::GetClientLogger()->warn(__VA_ARGS__)
#define WLD_ERROR(...) ::World::Log::GetClientLogger()->error(__VA_ARGS__)
#define WLD_CRITICAL(...) ::World::Log::GetClientLogger()->critical(__VA_ARGS__)
