#include "wldpch.h"
#include "Log.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <exception>

namespace World
{
	std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
	std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

	void Log::Init()
	{
		spdlog::set_pattern("%^[%T] %n: %v%$");
		// 未捕获异常/terminate 也要留下可读日志(否则只剩一个 abort() 对话框)。
		std::set_terminate([]()
		{
			try
			{
				throw;
			}
			catch (const std::exception& exception)
			{
				WLD_CORE_CRITICAL("std::terminate: unhandled exception: {0}", exception.what());
			}
			catch (...)
			{
				WLD_CORE_CRITICAL("std::terminate: unhandled non-standard exception");
			}
			std::abort();
		});
		s_CoreLogger = spdlog::stdout_color_mt("HAZEL");
		s_CoreLogger->set_level(spdlog::level::trace);
		// 断言/错误立即落盘:崩溃与断言对话框场景下 FIFO 缓冲会吞掉最后的日志。
		s_CoreLogger->flush_on(spdlog::level::err);

		s_ClientLogger = spdlog::stdout_color_mt("APP");
		s_ClientLogger->set_level(spdlog::level::trace);
		s_ClientLogger->flush_on(spdlog::level::err);
	}
}
