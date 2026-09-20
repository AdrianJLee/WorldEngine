#include "wldpch.h"
#include "Log.h"

#include "spdlog/sinks/base_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace World
{
	namespace
	{
		// 最近日志环形缓冲(AI 控制通道 log.tail 的数据源):
		// 只有 stdout 时,"运行时刚刚发生了什么"在自动化里完全拿不到。
		struct RecentLogBuffer
		{
			std::mutex Mutex;
			std::deque<std::string> Lines;
			size_t Capacity = 400;
		};

		RecentLogBuffer& Buffer()
		{
			static RecentLogBuffer buffer;
			return buffer;
		}

		// sink 只负责格式化,数据放在上面的进程级缓冲里(避免 sink 拷贝语义问题)。
		class RecentLinesSink : public spdlog::sinks::base_sink<std::mutex>
		{
		public:
		protected:
			void sink_it_(const spdlog::details::log_msg& msg) override
			{
				spdlog::memory_buf_t formatted;
				formatter_->format(msg, formatted);
				std::string line(formatted.data(), formatted.size());
				while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
					line.pop_back();
				RecentLogBuffer& buffer = Buffer();
				std::lock_guard<std::mutex> guard(buffer.Mutex);
				buffer.Lines.push_back(std::move(line));
				while (buffer.Lines.size() > buffer.Capacity)
					buffer.Lines.pop_front();
			}

			void flush_() override {}
		};
	}

	std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
	std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

	std::vector<std::string> Log::RecentLines(size_t maxLines)
	{
		RecentLogBuffer& buffer = Buffer();
		std::lock_guard<std::mutex> guard(buffer.Mutex);
		std::vector<std::string> lines;
		const size_t count = std::min(maxLines, buffer.Lines.size());
		lines.assign(buffer.Lines.end() - static_cast<std::ptrdiff_t>(count), buffer.Lines.end());
		return lines;
	}

	void Log::Init()
	{
		spdlog::set_pattern("%^[%T] %n: %v%$");
		// 最近日志缓冲:与 stdout sink 并存(不改既有输出行为)。
		std::shared_ptr<RecentLinesSink> recent = std::make_shared<RecentLinesSink>();
		recent->set_pattern("%^[%T] %n: %v%$");
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
		s_CoreLogger = spdlog::stdout_color_mt("WE");
		s_CoreLogger->set_level(spdlog::level::trace);
		s_CoreLogger->sinks().push_back(recent);
		// 断言/错误立即落盘:崩溃与断言对话框场景下 FIFO 缓冲会吞掉最后的日志。
		s_CoreLogger->flush_on(spdlog::level::err);

		s_ClientLogger = spdlog::stdout_color_mt("APP");
		s_ClientLogger->set_level(spdlog::level::trace);
		s_ClientLogger->flush_on(spdlog::level::err);

		// P4-UX2f:**默认写日志文件** —— 偶发 device lost / 崩溃时用户往往只看到控制台最后一行,
		// 而我们拿不到他们的控制台(AttachConsole 会被拒绝)。文件日志让下次复现自带现场:
		//   * 路径:WLD_LOG_FILE 指定,或默认 <WLD_OUTPUT_DIR><exe名>.log(5MB x 3 轮转);
		//   * WLD_LOG_FILE=0 关闭。
		const char* request = std::getenv("WLD_LOG_FILE");
		if (request && request[0] == '0' && request[1] == '\0')
			return;
		std::filesystem::path logPath;
		if (request && request[0])
			logPath = request;
		if (logPath.empty())
		{
			wchar_t modulePath[MAX_PATH] = {};
			const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
			std::filesystem::path exePath = length ? std::filesystem::path(modulePath) : std::filesystem::path("Editor");
			logPath = std::filesystem::path(WLD_OUTPUT_DIR) / (exePath.stem().string() + ".log");
		}
		try
		{
			auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
				logPath.string(), 5 * 1024 * 1024, 3);
			fileSink->set_pattern("%^[%Y-%m-%d %T] %n: %v%$");
			fileSink->set_level(spdlog::level::trace);
			s_CoreLogger->sinks().push_back(fileSink);
			s_ClientLogger->sinks().push_back(fileSink);
			WLD_CORE_INFO("日志文件: {0}", logPath.string());
		}
		catch (const std::exception& exception)
		{
			WLD_CORE_WARN("日志文件不可用({0}): {1}", logPath.string(), exception.what());
		}
	}
}
