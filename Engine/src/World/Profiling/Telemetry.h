#pragma once

#include "World/Core/Export.h"
#include "World/Profiling/TraceBuffer.h"
#include "World/Profiling/TraceEvents.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace World::Profiling
{
	// ---------------------------------------------------------------------------
	// Telemetry —— 采集门面(唯一真相来源)
	//
	// 两条独立通道,代价不同、开关不同:
	//
	//   A. **帧统计(常开)** —— 每帧 2 次取时 + 1 次直方图落桶,与作用域数无关。
	//      产出:帧时间 p50/p95/p99、掉帧数、FPS EMA。回答"这一帧慢不慢 / 稳不稳"。
	//   B. **作用域追踪(按需)** —— 采集窗口内每作用域一条事件,写进预分配缓冲。
	//      产出:层级作用域树 + 火焰图。回答"慢在哪"。
	//
	// "AI 和人都能看懂"的落点:四个消费者(面板 / AI 命令 / 导出文件 / 无障碍节点)
	// 全部只读同一个 StatsSnapshot,键名以 TraceEvents.h 与 docs/dev/profiling.md 为准,
	// **不允许各自计算**(见知识库 contract.telemetry-schema 的一致性规则)。
	// ---------------------------------------------------------------------------
	class WLD_API Telemetry
	{
	public:
		// ---- 帧统计 ----
		struct FrameStats
		{
			float LastMs = 0.0f;        // 最近一帧
			float MaxMs = 0.0f;         // 本次运行最大值
			float P50Ms = -1.0f;        // -1 = 样本不足
			float P95Ms = -1.0f;
			float P99Ms = -1.0f;
			float EmaFps = 0.0f;        // 指数移动平均 FPS(平滑,替代瞬时倒数)
			uint32_t SampleCount = 0;   // 已累计帧数
			uint32_t WindowFrames = 0;  // 当前分位数统计窗口内的样本数
			uint32_t JankCount = 0;     // 超过 2× 中位数的帧数
			float JankThresholdMs = -1.0f; // 当前掉帧判据(ms);-1 = 尚未建立
			bool Overflowed = false;    // 有样本落在 >128ms 的溢出桶
		};

		// ---- 采集状态 ----
		struct CaptureDesc
		{
			uint32_t Frames = 0;             // 采集帧数;0 = 一直采到 CancelCapture
			std::string OutputPath;          // 空 = 默认路径(见 Telemetry.cpp kDefaultTraceDir)
			bool IncludeCounters = true;     // 是否附带计数器事件
			uint32_t Capacity = 0;           // 0 = TraceBuffer::kDefaultCapacity
		};

		struct CaptureStatus
		{
			bool Active = false;
			bool HasResult = false;          // 上一次采集是否产出了文件
			uint32_t FramesRequested = 0;
			uint32_t FramesCaptured = 0;
			uint64_t DroppedEvents = 0;      // >0 ⇒ 本次采集不可用于定量结论
			uint64_t EventCount = 0;
			std::string OutputPath;
			std::string LastError;           // 非空 = 上次采集失败原因
		};

		// ---- 内存快照前的占位(M2 填充;语义见 telemetry-schema 契约)----
		struct MemoryStats
		{
			bool Collected = false;          // false ⇒ 下表字段全部为 -1(未采集,不是 0)
			int64_t CpuTotalBytes = -1;
			int64_t CpuPeakBytes = -1;
			int64_t GpuResidentBytes = -1;   // -1 = 后端未登记
		};

		// ---- 唯一真相 ----
		struct StatsSnapshot
		{
			uint64_t FrameIndex = 0;
			FrameStats Frame;
			MemoryStats Memory;
			CaptureStatus Capture;
			uint64_t ScopeEventsThisFrame = 0; // 本帧写入的事件数(0 = 未采集)
		};

		static Telemetry& Get();

		// ---- 生命周期(Application 驱动)----
		// 启动:读环境变量,WLD_TELEMETRY_CAPTURE=<frames> / WLD_TELEMETRY_OUT=<path>。
		static void Init();
		static void Shutdown();

		// ---- 帧边界(Application::Run 调用;必须成对)----
		static void BeginFrame();
		static void EndFrame();

		// ---- 作用域(宏包装,见 ProfilingMacros.h)----
		// 热路径:关闭时只有一次 relaxed 原子读 + 分支。
		static void PushScope(const char* name) noexcept;
		// 名字必须与对应的 PushScope 是**同一个指针**(宏传同一字面量) ⇒ 导出时靠指针相等配对。
		static void PopScope(const char* name) noexcept;
		static void SetCounter(const char* name, int64_t value) noexcept;
		static void SetThreadName(const char* name);

		// 周期性统计日志(仿 WLD_JOB_STATS 惯例)。用途:A/B 量采集开销、CI 里读数字,
		// 也让"人"在不接面板时能看到同一份数字。`WLD_TELEMETRY_STATS=1` 打开,
		// `WLD_TELEMETRY_STATS_SECONDS=<n>` 改间隔(默认 2 秒)。
		static bool StatsLogEnabled() noexcept { return Get().m_StatsLogEnabled; }

		// 最近帧时长(毫秒),**最旧在前、最新在后**。给面板画帧时间曲线用。
		// 返回实际写入条数;不分配、不排序。capacity 不足时返回**最近**的 capacity 帧。
		static size_t CollectFrameHistory(float* out, size_t capacity);
		static void LogStatsIfDue(double nowSeconds);
		// 采集是否开启(宏的快速门;relaxed 读)。
		static bool CaptureEnabled() noexcept
		{
			return Get().m_Enabled.load(std::memory_order_relaxed);
		}

		// ---- 采集控制 ----
		static bool RequestCapture(const CaptureDesc& desc);
		static void CancelCapture();
		static CaptureStatus GetCaptureStatus();

		// ---- 查询面 ----
		static StatsSnapshot GetStats();
		// 结构化 JSON:键名与 docs/dev/profiling.md 的数据字典一致(AI/脚本读这个)。
		static std::string DescribeStatsJson();
		// 纯文本摘要:人读,不依赖任何工具。
		static std::string DescribeStatsText();

	private:
		Telemetry() = default;
		Telemetry(const Telemetry&) = delete;
		Telemetry& operator=(const Telemetry&) = delete;

		void BeginFrameImpl();
		void EndFrameImpl();
		void BeginCapture(const CaptureDesc& desc);
		void FinishCapture();      // 落盘(只在采集结束后、帧循环之外)
		void ReadTelemetryEnv();   // 首帧读一次环境变量
		void RecordFrameSample(float ms);
		void RecomputePercentiles();
		float PercentileMs(double fraction) const;

		struct CaptureRuntime
		{
			TraceBuffer Buffer;
			bool Active = false;
			uint32_t FramesRequested = 0;
			uint32_t FramesCaptured = 0;
			uint64_t EventCount = 0;
			CaptureDesc Desc;
		};

		CaptureRuntime m_Capture;
		std::atomic<bool> m_Enabled { false };

		uint64_t m_FrameIndex = 0;
		uint64_t m_FrameStartUs = 0;
		uint64_t m_EventsThisFrame = 0;
		uint64_t m_FrameEventStart = 0;
		bool m_EnvChecked = false;
		bool m_StatsLogEnabled = false;
		// 周期性内存转储(WLD_MEMORY_DUMP=<秒>):把 Top 调用点写文件,供人/AI 事后分析。
		// 低频诊断,不在帧循环里做符号化(转储本身按间隔节流)。
		double m_MemoryDumpInterval = 0.0;
		double m_LastMemoryDump = 0.0;
		std::string m_MemoryDumpPath;
		uint32_t m_MemoryDumpCount = 0;
		double m_StatsLogInterval = 2.0;
		double m_LastStatsLog = 0.0;		bool m_Initialized = false;

		// 帧统计:环形最近样本 + 固定直方图(查询 O(桶数),无排序、无分配)。
		static constexpr uint32_t kFrameRingCapacity = 3600; // 60s @60fps
		// 固定直方图:分辨率 0.25ms、覆盖 0..128ms。分位数是**估计值**,量化到桶中心,
		// 因此查询时会被 MaxMs 夹住(估计值不可能超过实测最大值)。
		static constexpr uint32_t kHistogramBuckets = 512;   // 512 × 0.25ms = 0..128ms
		static constexpr float kHistogramBucketMs = 0.25f;

		float m_FrameRing[kFrameRingCapacity] = {};
		uint32_t m_FrameRingCount = 0;   // 已写入(上限 = 容量)
		uint32_t m_FrameRingWrite = 0;
		uint32_t m_Histogram[kHistogramBuckets] = {};
		uint32_t m_HistogramOverflow = 0;
		uint32_t m_HistogramSamples = 0;

		FrameStats m_Frame;
		uint32_t m_JankBaselineTimer = 0;

		MemoryStats m_Memory;
		CaptureStatus m_LastCapture;
	};

	// ---------------------------------------------------------------------------
	// TraceScope —— RAII 作用域(由 WLD_TRACE_SCOPE 宏构造;也可直接使用)。
	// 关闭采集时是零成本对象(只存一个指针)。
	// ---------------------------------------------------------------------------
	class TraceScope
	{
	public:
		explicit TraceScope(const char* name) noexcept
			: m_Name(name), m_Active(name != nullptr && Telemetry::CaptureEnabled())
		{
			if (m_Active)
				Telemetry::PushScope(m_Name);
		}

		~TraceScope()
		{
			if (m_Active)
				Telemetry::PopScope(m_Name);
		}

		TraceScope(const TraceScope&) = delete;
		TraceScope& operator=(const TraceScope&) = delete;

	private:
		const char* m_Name;
		bool m_Active;
	};
}
