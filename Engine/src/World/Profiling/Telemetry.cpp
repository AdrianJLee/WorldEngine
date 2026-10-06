#include "wldpch.h"
#include "World/Profiling/Telemetry.h"

#include "World/Core/Thread/JobSystem.h"
#include "World/Profiling/MemoryTrack.h"
#include "World/Profiling/TraceSink.h"
#include "World/Profiling/TraceWriter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace World::Profiling
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		uint64_t NowMicroseconds()
		{
			return static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(
					Clock::now().time_since_epoch()).count());
		}

		// 线程槽位:首次使用时分配,配 SetThreadName 给 trace 一个可读的 tid 名。
		// 上限 kMaxThreads(64) —— 超出者归入槽 0,不丢事件(只是聚合到主线程轨道)。
		thread_local uint16_t t_ThreadSlot = kInvalidThreadSlot;
		std::atomic<uint32_t> g_NextThreadSlot { 0 };

		const char* g_ThreadNames[kMaxThreads] = {};

		uint16_t EnsureThreadSlot()
		{
			if (t_ThreadSlot == kInvalidThreadSlot)
			{
				const uint32_t slot = g_NextThreadSlot.fetch_add(1, std::memory_order_relaxed);
				t_ThreadSlot = slot < kMaxThreads ? static_cast<uint16_t>(slot) : 0;
			}
			return t_ThreadSlot;
		}

		// 作用域深度是**每线程**的:并行系统各自维护自己的嵌套。
		thread_local uint16_t t_ScopeDepth = 0;

		// 采集窗口内收集到的名字指针 → 供导出时写线程名表用(仅采集线程写)。
		std::string TimestampSlug()
		{
			char buffer[64];
			const auto now = std::chrono::system_clock::now();
			const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
				now.time_since_epoch()).count();
			// 加序号:同一秒内二次采集不会互相覆盖。
			static std::atomic<uint32_t> serial { 0 };
			std::snprintf(buffer, sizeof(buffer), "%lld-%u", static_cast<long long>(seconds),
				serial.fetch_add(1, std::memory_order_relaxed));
			return buffer;
		}

		const char* kDefaultTraceDir = "telemetry";
	}

	Telemetry& Telemetry::Get()
	{
		static Telemetry instance;
		return instance;
	}

	// ---------------------------------------------------------------------------
	// 生命周期
	// ---------------------------------------------------------------------------

	void Telemetry::Init()
	{
		Telemetry& self = Get();
		if (self.m_Initialized)
			return;
		self.m_Initialized = true;
		// 内存归因先于帧统计启动:表在 Init 里一次性分配,之后采集路径零分配。
		// 默认开启(Debug/RelWithDebInfo 的诊断设施);WLD_MEMORY_TRACK=0 可关。
		{
			bool trackMemory = true;
			if (const char* env = std::getenv("WLD_MEMORY_TRACK"))
				trackMemory = !(env[0] == '0' && env[1] == '\0');
			if (trackMemory)
				MemoryTrack::Init();
		}
		EnsureThreadSlot();
		g_ThreadNames[EnsureThreadSlot()] = "Main";
		// 环境变量驱动(CI / 无头):在首帧统一读取,见 BeginFrameImpl。
	}

	void Telemetry::Shutdown()
	{
		Telemetry& self = Get();
		if (self.m_Capture.Active)
			self.FinishCapture();
		self.m_Capture.Buffer.Release();
		// 先于表销毁做泄漏/口径报告(旧实现把泄漏检查放在 Reset 之后,真泄漏已看不见)。
		MemoryTrack::Shutdown();
		self.m_Initialized = false;
	}

	// ---------------------------------------------------------------------------
	// 帧边界
	// ---------------------------------------------------------------------------

	void Telemetry::BeginFrame()
	{
		Get().BeginFrameImpl();
	}

	void Telemetry::EndFrame()
	{
		Get().EndFrameImpl();
	}

	void Telemetry::BeginFrameImpl()
	{
		if (!m_EnvChecked)
		{
			m_EnvChecked = true;
			ReadTelemetryEnv();
		}

		m_FrameStartUs = NowMicroseconds();
		m_EventsThisFrame = 0;
		m_FrameEventStart = m_Capture.Active ? m_Capture.Buffer.Size() : 0;

		if (m_Capture.Active)
		{
			TraceEvent event;
			event.Timestamp = m_FrameStartUs;
			event.FrameIndex = static_cast<uint32_t>(m_FrameIndex);
			event.ThreadSlot = EnsureThreadSlot();
			event.Depth = 0;
			event.Type = TraceEventType::FrameBegin;
			m_Capture.Buffer.Push(event);
		}
	}

	void Telemetry::EndFrameImpl()
	{
		const uint64_t endUs = NowMicroseconds();
		const float frameMs = (endUs > m_FrameStartUs)
			? static_cast<float>(static_cast<double>(endUs - m_FrameStartUs) / 1000.0)
			: 0.0f;

		if (m_Capture.Active)
		{
			TraceEvent event;
			event.Timestamp = endUs;
			event.FrameIndex = static_cast<uint32_t>(m_FrameIndex);
			event.ThreadSlot = EnsureThreadSlot();
			event.Depth = 0;
			event.Type = TraceEventType::FrameEnd;
			m_Capture.Buffer.Push(event);

			m_EventsThisFrame = m_Capture.Buffer.Size() - m_FrameEventStart;
			m_Capture.EventCount = m_Capture.Buffer.Size();
		}

		RecordFrameSample(frameMs);

		++m_FrameIndex;

		// 周期性统计输出(默认关;打开后每 N 秒一行)。
		// 内存采样由引擎每帧驱动(推动式),与"面板有没有打开"解耦。
		MemoryTrack::Tick();

		// 周期性内存转储(默认关):低频率,不在帧循环里做符号化。
		if (m_MemoryDumpInterval > 0.0)
		{
			const double nowSeconds = static_cast<double>(endUs) / 1000000.0;
			if (nowSeconds - m_LastMemoryDump >= m_MemoryDumpInterval)
			{
				m_LastMemoryDump = nowSeconds;
				char path[512];
				std::snprintf(path, sizeof(path), "%s-%03u.txt", m_MemoryDumpPath.c_str(), ++m_MemoryDumpCount);
				MemoryTrack::DumpReport(path);
			}
		}

		LogStatsIfDue(static_cast<double>(endUs) / 1000000.0);

		if (m_Capture.Active && m_Capture.FramesRequested > 0)
		{
			++m_Capture.FramesCaptured;
			if (m_Capture.FramesCaptured >= m_Capture.FramesRequested)
				FinishCapture();
		}
	}

	void Telemetry::RecordFrameSample(float ms)
	{
		m_Frame.LastMs = ms;
		if (ms > m_Frame.MaxMs)
			m_Frame.MaxMs = ms;

		m_FrameRing[m_FrameRingWrite] = ms;
		m_FrameRingWrite = (m_FrameRingWrite + 1) % kFrameRingCapacity;
		if (m_FrameRingCount < kFrameRingCapacity)
			++m_FrameRingCount;

		const uint32_t bucket = static_cast<uint32_t>(ms / kHistogramBucketMs);
		if (bucket < kHistogramBuckets && ms >= 0.0f)
			++m_Histogram[bucket];
		else
		{
			++m_HistogramOverflow;
			m_Frame.Overflowed = true;
		}
		++m_HistogramSamples;

		const double instantFps = ms > 0.0f ? (1000.0 / static_cast<double>(ms)) : 0.0;
		// 平滑:单帧瞬时倒数会高频抖动,面板上读不出趋势(旧实现的问题)。
		m_Frame.EmaFps = m_Frame.EmaFps <= 0.0f
			? static_cast<float>(instantFps)
			: static_cast<float>(m_Frame.EmaFps * 0.9 + instantFps * 0.1);

		m_Frame.SampleCount = m_HistogramSamples;
		m_Frame.WindowFrames = m_FrameRingCount;

		// 掉帧判据按低频重算(每 120 帧),不与每帧的采样成本耦合。
		// 启动头 2 秒(样本未满)每帧重算,避免面板开局显示 -1;之后降到每 120 帧。
		if (++m_JankBaselineTimer >= 120 || m_Frame.JankThresholdMs < 0.0f || m_HistogramSamples < 120)
		{
			m_JankBaselineTimer = 0;
			RecomputePercentiles();
			const float threshold = m_Frame.P50Ms > 0.0f ? m_Frame.P50Ms * 2.0f : -1.0f;
			if (m_Frame.JankThresholdMs < 0.0f && threshold > 0.0f)
				m_Frame.JankThresholdMs = threshold;
			else if (threshold > 0.0f)
				m_Frame.JankThresholdMs = threshold;
		}

		if (m_Frame.JankThresholdMs > 0.0f && ms > m_Frame.JankThresholdMs)
			++m_Frame.JankCount;
	}

	void Telemetry::RecomputePercentiles()
	{
		m_Frame.P50Ms = PercentileMs(0.50);
		m_Frame.P95Ms = PercentileMs(0.95);
		m_Frame.P99Ms = PercentileMs(0.99);
	}

	float Telemetry::PercentileMs(double fraction) const
	{
		if (m_HistogramSamples == 0)
			return -1.0f;

		const double target = fraction * static_cast<double>(m_HistogramSamples);
		uint64_t cumulative = 0;
		for (uint32_t bucket = 0; bucket < kHistogramBuckets; ++bucket)
		{
			cumulative += m_Histogram[bucket];
			if (static_cast<double>(cumulative) >= target)
			{
				// 返回桶中心,避免系统性偏低(用桶下界会让 p95 看起来更好看)。
				const float estimate = (static_cast<float>(bucket) + 0.5f) * kHistogramBucketMs;
				// 夹到实测最大值:分位数是量化估计,不该报出比 MaxMs 还大的数(看起来像 bug)。
				return (m_Frame.MaxMs > 0.0f && estimate > m_Frame.MaxMs) ? m_Frame.MaxMs : estimate;
			}
		}
		// 落在 >128ms 溢出桶:返回桶下界作为**下界**估计,并用 Overflowed 标出不确定性。
		return static_cast<float>(kHistogramBuckets) * kHistogramBucketMs;
	}

	// ---------------------------------------------------------------------------
	// 作用域与计数器
	// ---------------------------------------------------------------------------

	void Telemetry::PushScope(const char* name) noexcept
	{
		Telemetry& self = Get();
		if (!self.m_Capture.Active)
			return;

		TraceEvent event;
		event.Timestamp = NowMicroseconds();
		event.Name = name;
		event.FrameIndex = static_cast<uint32_t>(self.m_FrameIndex);
		event.ThreadSlot = EnsureThreadSlot();
		event.Depth = t_ScopeDepth;
		event.Type = TraceEventType::ScopeBegin;
		++t_ScopeDepth;
		self.m_Capture.Buffer.Push(event);
	}

	void Telemetry::PopScope(const char* name) noexcept
	{
		Telemetry& self = Get();
		if (!self.m_Capture.Active)
			return;

		if (t_ScopeDepth > 0)
			--t_ScopeDepth;

		TraceEvent event;
		event.Timestamp = NowMicroseconds();
		// Name 用于配对:Push/Pop 传的是同一个字面量指针,导出时靠指针相等匹配同名作用域。
		event.Name = name;
		event.FrameIndex = static_cast<uint32_t>(self.m_FrameIndex);
		event.ThreadSlot = EnsureThreadSlot();
		event.Depth = t_ScopeDepth;
		event.Type = TraceEventType::ScopeEnd;
		self.m_Capture.Buffer.Push(event);
	}

	void Telemetry::SetCounter(const char* name, int64_t value) noexcept
	{
		Telemetry& self = Get();
		if (!self.m_Capture.Active || !self.m_Capture.Desc.IncludeCounters)
			return;

		TraceEvent event;
		event.Timestamp = NowMicroseconds();
		event.Name = name;
		event.Value = value;
		event.FrameIndex = static_cast<uint32_t>(self.m_FrameIndex);
		event.ThreadSlot = EnsureThreadSlot();
		event.Type = TraceEventType::Counter;
		self.m_Capture.Buffer.Push(event);
	}

	void Telemetry::SetThreadName(const char* name)
	{
		if (!name || name[0] == '\0')
			return;
		const uint16_t slot = EnsureThreadSlot();
		g_ThreadNames[slot] = name;
	}

	// ---------------------------------------------------------------------------
	// 采集控制
	// ---------------------------------------------------------------------------

	bool Telemetry::RequestCapture(const CaptureDesc& desc)
	{
		Telemetry& self = Get();
		if (self.m_Capture.Active)
			return false;

		self.BeginCapture(desc);
		return true;
	}

	void Telemetry::BeginCapture(const CaptureDesc& desc)
	{
		const uint32_t capacity = desc.Capacity > 0 ? desc.Capacity : TraceBuffer::kDefaultCapacity;
		if (!m_Capture.Buffer.EnsureAllocated(capacity))
		{
			m_LastCapture = {};
			m_LastCapture.LastError = "telemetry capture buffer allocation failed";
			WLD_CORE_ERROR("[telemetry] {0}", m_LastCapture.LastError);
			return;
		}

		m_Capture.Buffer.Reset();
		m_Capture.Desc = desc;
		m_Capture.FramesRequested = desc.Frames;
		m_Capture.FramesCaptured = 0;
		m_Capture.EventCount = 0;
		m_Capture.Active = true;

		m_LastCapture = {};
		m_LastCapture.Active = true;
		m_LastCapture.FramesRequested = desc.Frames;
		m_LastCapture.OutputPath = desc.OutputPath;

		// 允许写者开闸。放在最后:缓冲已就绪、状态已一致。
		m_Enabled.store(true, std::memory_order_release);
		WLD_CORE_INFO("[telemetry] capture started (frames={0}, capacity={1})",
			desc.Frames, capacity);
	}

	void Telemetry::CancelCapture()
	{
		Telemetry& self = Get();
		if (!self.m_Capture.Active)
			return;

		self.m_Enabled.store(false, std::memory_order_release);
		self.m_Capture.Active = false;
		self.m_Capture.Buffer.Reset();
		self.m_LastCapture.Active = false;
		self.m_LastCapture.LastError = "capture cancelled";
	}

	void Telemetry::FinishCapture()
	{
		// 先关闸,再读缓冲 —— 这是本模块唯一的顺序约束(见 TraceBuffer 的线程契约)。
		// 关闸只保证此后无人再写;已在飞的 job 必须等完,否则会读到半写事件。
		m_Enabled.store(false, std::memory_order_release);
		m_Capture.Active = false;
		JobSystem::WaitAll();

		const uint32_t eventCount = m_Capture.Buffer.Size();
		const uint64_t dropped = m_Capture.Buffer.Dropped();

		std::string path = m_Capture.Desc.OutputPath;
		if (path.empty())
		{
			std::error_code ec;
			std::filesystem::create_directories(kDefaultTraceDir, ec);
			path = std::string(kDefaultTraceDir) + "/trace-" + TimestampSlug() + ".json";
		}

		TraceMeta meta[8];
		uint32_t metaCount = 0;
		meta[metaCount++] = { "process_name", "WorldEngine" };
		meta[metaCount++] = { "trace_sink", ActiveTraceSinkName() };
		const char* backend = std::getenv("WLD_TELEMETRY_BACKEND");
		if (backend && backend[0] != '\0')
			meta[metaCount++] = { "backend", backend };

		TraceWriteRequest request;
		request.Events = m_Capture.Buffer.Data();
		request.EventCount = eventCount;
		request.DroppedEvents = dropped;
		request.Meta = meta;
		request.MetaCount = metaCount;
		for (uint32_t slot = 0; slot < kMaxThreads; ++slot)
			request.ThreadNames[slot] = g_ThreadNames[slot];
		request.ThreadNameCount = kMaxThreads;

		std::string error;
		// 经 sink 写出:默认 = Chrome Trace;换后端只换 sink,插桩点不动(见 TraceSink.h)。
		const bool ok = GetTraceSink()->Write(path, request, &error);

		m_LastCapture.Active = false;
		m_LastCapture.HasResult = ok;
		m_LastCapture.FramesRequested = m_Capture.FramesRequested;
		m_LastCapture.FramesCaptured = m_Capture.FramesCaptured;
		m_LastCapture.DroppedEvents = dropped;
		m_LastCapture.EventCount = eventCount;
		m_LastCapture.OutputPath = path;
		m_LastCapture.LastError = ok ? std::string() : error;

		if (ok)
		{
			WLD_CORE_INFO("[telemetry] capture finished: {0} events ({1} dropped), {2} frames -> {3}",
				eventCount, dropped, m_Capture.FramesCaptured, path);
			if (dropped > 0)
				WLD_CORE_WARN("[telemetry] {0} event(s) dropped: trace is incomplete", dropped);
		}
		else
		{
			WLD_CORE_ERROR("[telemetry] capture failed: {0}", error);
		}

		m_Capture.Desc = {};
		m_Capture.FramesRequested = 0;
		m_Capture.EventCount = 0;
		m_Capture.Buffer.Release();
	}

	void Telemetry::ReadTelemetryEnv()
	{
		// 统计日志开关与间隔(与采集开关独立:可以只观测不采集)。
		if (const char* statsEnv = std::getenv("WLD_TELEMETRY_STATS"))
			m_StatsLogEnabled = statsEnv[0] != '\0' && statsEnv[0] != '0';
		if (const char* intervalEnv = std::getenv("WLD_TELEMETRY_STATS_SECONDS"))
		{
			const double seconds = std::strtod(intervalEnv, nullptr);
			if (seconds > 0.0)
				m_StatsLogInterval = seconds;
		}

		// 周期性内存转储(默认关)。WLD_MEMORY_DUMP=<秒>;
		// WLD_MEMORY_DUMP_PATH 可指定路径,默认 telemetry/memory-dump-<n>.txt。
		if (const char* dumpEnv = std::getenv("WLD_MEMORY_DUMP"))
		{
			const double seconds = std::strtod(dumpEnv, nullptr);
			if (seconds > 0.0)
			{
				m_MemoryDumpInterval = seconds;
				m_MemoryDumpPath = "telemetry/memory-dump";
				if (const char* pathEnv = std::getenv("WLD_MEMORY_DUMP_PATH"))
					if (pathEnv[0] != '\0')
						m_MemoryDumpPath = pathEnv;
				std::error_code ec;
				if (const std::filesystem::path parent = std::filesystem::path(m_MemoryDumpPath).parent_path(); !parent.empty())
					std::filesystem::create_directories(parent, ec);
			}
		}

		// 采集后端选择。当前只有内置;`WLD_TELEMETRY_SINK` 是给将来注册外部 sink 留的同一入口。
		if (const char* sinkEnv = std::getenv("WLD_TELEMETRY_SINK"))
		{
			if (sinkEnv[0] != '\0' && std::strcmp(sinkEnv, "chrome-trace") != 0)
				WLD_CORE_WARN("[telemetry] requested sink '{0}' is not registered; using '{1}'",
					sinkEnv, ActiveTraceSinkName());
		}

		const char* framesEnv = std::getenv("WLD_TELEMETRY_CAPTURE");
		if (!framesEnv || framesEnv[0] == '\0' || framesEnv[0] == '0')
			return;

		const long frames = std::strtol(framesEnv, nullptr, 10);
		if (frames <= 0)
			return;

		CaptureDesc desc;
		desc.Frames = static_cast<uint32_t>(frames);
		if (const char* out = std::getenv("WLD_TELEMETRY_OUT"); out && out[0] != '\0')
			desc.OutputPath = out;

		// 环境变量驱动的采集是 CI/无头的主通道:无条件启动,失败也明确报错。
		WLD_CORE_INFO("[telemetry] WLD_TELEMETRY_CAPTURE={0} -> starting capture", frames);
		BeginCapture(desc);
	}

	void Telemetry::LogStatsIfDue(double nowSeconds)
	{
		Telemetry& self = Get();
		if (!self.m_StatsLogEnabled)
			return;
		if (nowSeconds - self.m_LastStatsLog < self.m_StatsLogInterval)
			return;
		self.m_LastStatsLog = nowSeconds;
		// 一行,含分位数与掉帧 —— 人可读,同时脚本可正则取值做 A/B 与阈值断言。
		WLD_CORE_INFO("[telemetry] {0} capture={1} dropped={2}",
			DescribeStatsText(), self.m_Capture.Active ? "on" : "off",
			self.m_Capture.Active ? self.m_Capture.Buffer.Dropped() : self.m_LastCapture.DroppedEvents);
	}

	size_t Telemetry::CollectFrameHistory(float* out, size_t capacity)
	{
		if (!out || capacity == 0)
			return 0;
		Telemetry& self = Get();
		const uint32_t available = self.m_FrameRingCount;
		const size_t count = available < capacity ? available : capacity;
		// 环形展开:把"最近 count 帧"按时间顺序摊平(最后一个是当前帧)。
		const uint32_t start = available < kFrameRingCapacity
			? static_cast<uint32_t>(available - count)
			: static_cast<uint32_t>((self.m_FrameRingWrite + kFrameRingCapacity - count) % kFrameRingCapacity);
		for (size_t i = 0; i < count; ++i)
			out[i] = self.m_FrameRing[(start + i) % kFrameRingCapacity];
		return count;
	}

	Telemetry::CaptureStatus Telemetry::GetCaptureStatus()
	{
		return Get().m_LastCapture;
	}

	// ---------------------------------------------------------------------------
	// 查询面
	// ---------------------------------------------------------------------------

	Telemetry::StatsSnapshot Telemetry::GetStats()
	{
		Telemetry& self = Get();

		// 面板每帧都要一份;这里不重算分位数(那是每 120 帧一次的低频工作),
		// 只做一次廉价拷贝 —— 面板/AI/导出/a11y 读到的因此是**同一份数字**。
		StatsSnapshot snapshot;
		snapshot.FrameIndex = self.m_FrameIndex;
		snapshot.Frame = self.m_Frame;
		snapshot.Memory = self.m_Memory;
		// 内存面:同一份快照供面板 / AI 命令 / 导出 / 无障碍四处使用(单一真相)。
		{
			const uint64_t heapLive = MemoryTrack::HeapLiveBytes();
			const uint64_t pools = MemoryTrack::PoolUsedBytes();
			snapshot.Memory.Collected = MemoryTrack::Enabled();
			snapshot.Memory.CpuTotalBytes = static_cast<int64_t>(heapLive + pools);
			snapshot.Memory.CpuPeakBytes = static_cast<int64_t>(MemoryTrack::HeapPeakBytes() + pools);
			snapshot.Memory.GpuResidentBytes = static_cast<int64_t>(MemoryTrack::GpuResidentBytes());
		}
		snapshot.Capture = self.m_LastCapture;
		if (self.m_Capture.Active)
		{
			snapshot.Capture.Active = true;
			snapshot.Capture.FramesRequested = self.m_Capture.FramesRequested;
			snapshot.Capture.FramesCaptured = self.m_Capture.FramesCaptured;
			snapshot.Capture.EventCount = self.m_Capture.Buffer.Size();
			snapshot.Capture.DroppedEvents = self.m_Capture.Buffer.Dropped();
			snapshot.Capture.OutputPath = self.m_Capture.Desc.OutputPath;
			snapshot.ScopeEventsThisFrame = self.m_EventsThisFrame;
		}
		return snapshot;
	}

	std::string Telemetry::DescribeStatsJson()
	{
		const StatsSnapshot snapshot = GetStats();
		char buffer[2048];
		std::snprintf(buffer, sizeof(buffer),
			"{"
			"\"frameIndex\":%llu,"
			"\"frameMs\":{\"last\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,"
			"\"samples\":%u,\"window\":%u,\"jank\":%u,\"jankThresholdMs\":%.3f,\"overflowed\":%s},"
			"\"fps\":{\"ema\":%.2f},"
			"\"mem\":{\"collected\":%s,\"cpuTotal\":%lld,\"cpuPeak\":%lld,\"gpuResident\":%lld},"
			"\"capture\":{\"active\":%s,\"hasResult\":%s,\"framesRequested\":%u,\"framesCaptured\":%u,"
			"\"events\":%llu,\"droppedEvents\":%llu,\"path\":\"%s\",\"error\":\"%s\"}"
			"}",
			static_cast<unsigned long long>(snapshot.FrameIndex),
			snapshot.Frame.LastMs, snapshot.Frame.P50Ms, snapshot.Frame.P95Ms,
			snapshot.Frame.P99Ms, snapshot.Frame.MaxMs,
			snapshot.Frame.SampleCount, snapshot.Frame.WindowFrames,
			snapshot.Frame.JankCount, snapshot.Frame.JankThresholdMs,
			snapshot.Frame.Overflowed ? "true" : "false",
			snapshot.Frame.EmaFps,
			snapshot.Memory.Collected ? "true" : "false",
			static_cast<long long>(snapshot.Memory.CpuTotalBytes),
			static_cast<long long>(snapshot.Memory.CpuPeakBytes),
			static_cast<long long>(snapshot.Memory.GpuResidentBytes),
			snapshot.Capture.Active ? "true" : "false",
			snapshot.Capture.HasResult ? "true" : "false",
			snapshot.Capture.FramesRequested, snapshot.Capture.FramesCaptured,
			static_cast<unsigned long long>(snapshot.Capture.EventCount),
			static_cast<unsigned long long>(snapshot.Capture.DroppedEvents),
			snapshot.Capture.OutputPath.c_str(),
			snapshot.Capture.LastError.c_str());
		return buffer;
	}

	std::string Telemetry::DescribeStatsText()
	{
		const StatsSnapshot snapshot = GetStats();
		// 内存摘要与帧统计同源(人读的一行;AI 读 DescribeStatsJson)。
		std::string result = MemoryTrack::DescribeText();
		// 先构造再取 c_str():写在条件表达式里的临时串会在 snprintf 之前析构。
		const std::string progress = snapshot.Capture.Active
			? (" (" + std::to_string(snapshot.Capture.FramesCaptured) + "/" +
			   std::to_string(snapshot.Capture.FramesRequested) + " frames)")
			: std::string();
		char buffer[1024];
		std::snprintf(buffer, sizeof(buffer),
			"Frame %llu: %.2f ms (p50 %.2f / p95 %.2f / p99 %.2f, max %.2f)  %.1f FPS  jank %u\n"
			"Capture: %s%s\n",
			static_cast<unsigned long long>(snapshot.FrameIndex),
			snapshot.Frame.LastMs, snapshot.Frame.P50Ms, snapshot.Frame.P95Ms,
			snapshot.Frame.P99Ms, snapshot.Frame.MaxMs, snapshot.Frame.EmaFps,
			snapshot.Frame.JankCount,
			snapshot.Capture.Active ? "active" : "idle",
			progress.c_str());
		result += buffer;
		return result;
	}
}
