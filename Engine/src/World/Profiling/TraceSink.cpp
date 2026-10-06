#include "wldpch.h"
#include "World/Profiling/TraceSink.h"

#include <atomic>

namespace World::Profiling
{
	namespace
	{
		// 内置后端:Chrome Trace Event JSON(Perfetto / chrome://tracing 直接可看)。
		// 它是**默认**且始终存在 —— 即使外部 sink 注册失败,也总有可用的落地格式。
		class ChromeTraceSink final : public ITraceSink
		{
		public:
			const char* Name() const override { return "chrome-trace"; }
			bool Write(const std::string& path, const TraceWriteRequest& request, std::string* error) override
			{
				return WriteChromeTrace(path, request, error);
			}
		};

		ChromeTraceSink& DefaultSink()
		{
			static ChromeTraceSink sink;
			return sink;
		}

		std::atomic<ITraceSink*> g_Active { nullptr };
	}

	void SetTraceSink(ITraceSink* sink)
	{
		// 只换指针,不接管生命周期:外部 sink 由注册方保证在本进程存活期有效
		// (通常是静态对象)。这样避免 Telemetry 成为所有者而在关停顺序上打架。
		g_Active.store(sink, std::memory_order_release);
	}

	ITraceSink* GetTraceSink()
	{
		ITraceSink* sink = g_Active.load(std::memory_order_acquire);
		return sink ? sink : &DefaultSink();
	}

	const char* ActiveTraceSinkName()
	{
		return GetTraceSink()->Name();
	}
}
