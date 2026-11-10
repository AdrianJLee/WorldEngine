// 遥测(M1)回归:帧统计分位数、采集落盘与回读、缓冲溢出计数、并发写入、复用不增长。
//
// 反假通过(见知识库 verification/profiling-bar):
//   * 分位数**不是**只断言 "p95 > 0" —— 构造"多数快帧 + 少数慢帧"的已知形状,
//     断言慢帧把 p99 抬高、且 p50 仍在快帧一侧(实现若返回常数或忽略尾部会变红);
//   * 采集回读不止断言"文件存在" —— 解析 JSON 后逐项核对 schemaVersion/帧数/事件数;
//   * 溢出必须**可计数**(droppedEvents),不允许静默截断;
//   * 复用:二次采集的缓冲字节数不增长。
#include "World/Core/Thread/JobSystem.h"
#include "World/Gameplay/Runtime/SystemRegistry.h"
#include "World/Profiling/MemoryTrack.h"
#include "World/Profiling/ProfilingMacros.h"
#include "World/Profiling/Telemetry.h"
#include "World/Profiling/TraceBuffer.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
	constexpr int kConcurrentThreads = 4;
	constexpr int kScopesPerThread = 200;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	using namespace World::Profiling;

	void SleepMs(int milliseconds)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
	}

	// 跑一帧:开边界 → 可选的"帧内工作"(模拟慢帧)→ 若干作用域 → 关边界。
	// 工作放在**作用域之外**:否则 workMs 会被乘以作用域数,慢帧时长不再可控
	// (首版就是踩了这个坑,导致分位数断言建立在错误的形状上)。
	void RunFrame(int workMs = 0, int scopeCount = 3)
	{
		Telemetry::BeginFrame();
		if (workMs > 0)
			SleepMs(workMs);
		for (int i = 0; i < scopeCount; ++i)
		{
			WLD_TRACE_SCOPE("telemetry.test.scope");
		}
		Telemetry::EndFrame();
	}

	std::string ReadFileText(const std::string& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			throw std::runtime_error("cannot read trace file: " + path);
		std::ostringstream buffer;
		buffer << stream.rdbuf();
		return buffer.str();
	}

	// 极简计数:从 JSON 文本里数某个键出现次数(测试不需要完整解析器)。
	size_t CountOccurrences(const std::string& text, const std::string& needle)
	{
		size_t count = 0;
		for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
			++count;
		return count;
	}

	bool Contains(const std::string& text, const std::string& needle)
	{
		return text.find(needle) != std::string::npos;
	}
}

int main()
{
	try
	{
		Telemetry::Init();

		// ---------------------------------------------------------------
		// A. 已知形状的分位数(反假:实现若返回常数/忽略尾部会失败)
		// ---------------------------------------------------------------
		{
			// 形状:60 帧"快"(无帧内工作,亚毫秒)+ 8 帧"慢"(50ms)。
			// 慢帧占 8/68 ≈ 11.8% ⇒ p50 必须落在快帧一侧,p95/p99 必须被慢帧抬到慢区。
			for (int i = 0; i < 60; ++i)
				RunFrame(0);
			for (int i = 0; i < 8; ++i)
				RunFrame(50);

			const Telemetry::StatsSnapshot snapshot = Telemetry::GetStats();
			std::printf("A: p50=%.2f p95=%.2f p99=%.2f max=%.2f jank=%u samples=%u\n",
				snapshot.Frame.P50Ms, snapshot.Frame.P95Ms, snapshot.Frame.P99Ms,
				snapshot.Frame.MaxMs, snapshot.Frame.JankCount, snapshot.Frame.SampleCount);
			CHECK(snapshot.Frame.SampleCount == 68);
			CHECK(snapshot.Frame.P50Ms > 0.0f);
			CHECK(snapshot.Frame.P50Ms < 5.0f);            // 中位数在快帧一侧
			CHECK(snapshot.Frame.P95Ms >= 20.0f);          // 尾部被慢帧抬高
			CHECK(snapshot.Frame.P99Ms >= 20.0f);
			CHECK(snapshot.Frame.P99Ms >= snapshot.Frame.P95Ms);
			CHECK(snapshot.Frame.P95Ms >= snapshot.Frame.P50Ms);
			CHECK(snapshot.Frame.MaxMs >= 45.0f);
			// 分离度:实现若返回常数、或把尾部丢掉,p99/p50 会塌到 1 附近。
			CHECK(snapshot.Frame.P99Ms > snapshot.Frame.P50Ms * 4.0f);
			CHECK(snapshot.Frame.JankCount > 0);
			CHECK(snapshot.Frame.EmaFps > 0.0f);
			// 分位数是"最近窗口"口径:样本数不超过环形容量。
			CHECK(snapshot.Frame.WindowFrames <= 3600);
		}

		// ---------------------------------------------------------------
		// B. 采集 N 帧 → 落盘 → 回读核对
		// ---------------------------------------------------------------
		const std::string tracePath = "tmp/telemetry-tests/capture.json";
		std::error_code ec;
		std::filesystem::remove_all("tmp/telemetry-tests", ec);
		{
			Telemetry::CaptureDesc desc;
			desc.Frames = 4;
			desc.OutputPath = tracePath;
			CHECK(Telemetry::RequestCapture(desc));
			// 采集进行中不能再开一次(单采集槽)。
			CHECK(!Telemetry::RequestCapture(desc));

			for (int i = 0; i < 4; ++i)
				RunFrame(0, 4);

			const Telemetry::CaptureStatus status = Telemetry::GetCaptureStatus();
			CHECK(!status.Active);
			CHECK(status.HasResult);
			CHECK(status.FramesCaptured == 4);
			CHECK(status.DroppedEvents == 0);
			CHECK(status.EventCount > 0);
			CHECK(status.LastError.empty());
			CHECK(status.OutputPath == tracePath);

			const std::string text = ReadFileText(tracePath);
			CHECK(Contains(text, "\"schemaVersion\":2"));
			CHECK(Contains(text, "\"droppedEvents\":0"));
			CHECK(Contains(text, "\"process_name\""));
			CHECK(Contains(text, "\"thread_name\""));
			// 每帧一条 Frame 事件(4 帧)——没有帧边界,trace 无法按帧切片。
			CHECK(CountOccurrences(text, "\"name\":\"Frame\"") == 4);
			// 每帧一条帧时间计数器 ⇒ Perfetto 里能看到帧时间曲线。
			CHECK(CountOccurrences(text, "\"name\":\"frameUs\"") == 4);
			// 4 帧 × 4 个作用域 = 16 条作用域事件。
			CHECK(CountOccurrences(text, "\"cat\":\"scope\"") == 16);
			CHECK(Contains(text, "telemetry.test.scope"));
		}

		// ---------------------------------------------------------------
		// C. 容量溢出必须可计数(不允许静默截断)
		// ---------------------------------------------------------------
		{
			Telemetry::CaptureDesc desc;
			desc.Frames = 3;
			desc.OutputPath = "tmp/telemetry-tests/overflow.json";
			desc.Capacity = TraceBuffer::kSmallCapacity; // 4096:每帧推入远超它的事件数
			CHECK(Telemetry::RequestCapture(desc));
			for (int frame = 0; frame < 3; ++frame)
			{
				Telemetry::BeginFrame();
				for (int i = 0; i < 4000; ++i)
				{
					WLD_TRACE_SCOPE("telemetry.test.flood");
				}
				Telemetry::EndFrame();
			}
			const Telemetry::CaptureStatus status = Telemetry::GetCaptureStatus();
			CHECK(status.HasResult);
			CHECK(status.DroppedEvents > 0); // 溢出被看见,而不是假装完整
			const std::string text = ReadFileText(status.OutputPath);
			CHECK(Contains(text, "\"droppedEvents\":"));
			CHECK(!Contains(text, "\"droppedEvents\":0"));
		}

		// ---------------------------------------------------------------
		// D. 并发写入:工作线程同时插桩不崩、事件不丢
		// ---------------------------------------------------------------
		{
			Telemetry::CaptureDesc desc;
			desc.Frames = 1;
			desc.OutputPath = "tmp/telemetry-tests/concurrent.json";
			CHECK(Telemetry::RequestCapture(desc));

			Telemetry::BeginFrame();
			std::vector<std::thread> workers;
			for (int t = 0; t < kConcurrentThreads; ++t)
				workers.emplace_back([]()
					{
						WLD_TRACE_THREAD_NAME("Worker");
						for (int i = 0; i < kScopesPerThread; ++i)
						{
							WLD_TRACE_SCOPE("telemetry.test.parallel");
						}
					});
			for (std::thread& worker : workers)
				worker.join();
			Telemetry::EndFrame();

			const Telemetry::CaptureStatus status = Telemetry::GetCaptureStatus();
			CHECK(status.HasResult);
			CHECK(status.DroppedEvents == 0);
			const std::string text = ReadFileText(status.OutputPath);
			CHECK(CountOccurrences(text, "telemetry.test.parallel") == kConcurrentThreads * kScopesPerThread);
		}

		// ---------------------------------------------------------------
		// E. 缓冲复用:二次采集不再增长(采集期分配只发生一次)
		// ---------------------------------------------------------------
		{
			Telemetry::CaptureDesc desc;
			desc.Frames = 1;
			desc.OutputPath = "tmp/telemetry-tests/reuse-a.json";
			CHECK(Telemetry::RequestCapture(desc));
			RunFrame(0, 2);
			const uint64_t firstEvents = Telemetry::GetCaptureStatus().EventCount;
			CHECK(firstEvents > 0);

			desc.OutputPath = "tmp/telemetry-tests/reuse-b.json";
			CHECK(Telemetry::RequestCapture(desc));
			RunFrame(0, 2);
			const uint64_t secondEvents = Telemetry::GetCaptureStatus().EventCount;
			// 同样的帧形状 ⇒ 事件数应一致(缓冲清空可复用,不是"越攒越多")。
			CHECK(secondEvents == firstEvents);
		}

		// ---------------------------------------------------------------
		// F. 取消:不产出文件,且状态可见
		// ---------------------------------------------------------------
		{
			Telemetry::CaptureDesc desc;
			desc.Frames = 1000;
			desc.OutputPath = "tmp/telemetry-tests/cancelled.json";
			CHECK(Telemetry::RequestCapture(desc));
			RunFrame(0, 1);
			CHECK(Telemetry::GetCaptureStatus().Active);
			Telemetry::CancelCapture();
			CHECK(!Telemetry::GetCaptureStatus().Active);
			CHECK(!std::filesystem::exists("tmp/telemetry-tests/cancelled.json"));
		}

		// ---------------------------------------------------------------
		// G. 机器可读查询面(AI 读这个)
		// ---------------------------------------------------------------
		{
			const Telemetry::StatsSnapshot snapshotMemory = Telemetry::GetStats();
			const std::string json = Telemetry::DescribeStatsJson();
			CHECK(Contains(json, "\"frameIndex\":"));
			CHECK(Contains(json, "\"frameMs\":{"));
			CHECK(Contains(json, "\"p99\":"));
			CHECK(Contains(json, "\"jank\":"));
			// M2 起内存面真正采集(不再是占位):collected=true 且数值有效。
			CHECK(Contains(json, "\"mem\":{\"collected\":true"));
			CHECK(snapshotMemory.Memory.Collected);
			CHECK(snapshotMemory.Memory.CpuTotalBytes >= 0);
			CHECK(snapshotMemory.Memory.GpuResidentBytes >= 0);
			CHECK(Contains(json, "\"capture\":{"));
			const std::string text = Telemetry::DescribeStatsText();
			CHECK(Contains(text, "Frame "));
		}

		// ---------------------------------------------------------------
		// H. 帧系统插桩:系统名必须作为作用域事件出现。
		//    这一条覆盖 e2e 快照覆盖不到的部分 —— 示例项目的场景没有注册帧系统,
		//    所以"系统作用域"只能在这里被证明;并行批次还会跑到 worker 线程上,
		//    顺带证明 worker 线程的插桩(旧 Instrumentor 正是死在这里)。
		// ---------------------------------------------------------------
		{
			using namespace World::Gameplay;
			World::JobSystem::Init();
			CHECK(World::JobSystem::IsRunning());

			SystemRegistry registry;
			SystemDesc serial;
			serial.Name = "telemetry.test.system.serial";
			serial.Phase = SystemPhase::Update;
			CHECK(registry.Register(serial, [](World::Timestep) {}));

			// 两个声明了**不相交**读写集的系统 ⇒ 同批并行(见 SystemRegistry 的冲突判定)。
			SystemDesc parallelA;
			parallelA.Name = "telemetry.test.system.parallelA";
			parallelA.Phase = SystemPhase::Update;
			parallelA.ParallelSafe = true;
			CHECK(registry.Register(parallelA, [](World::Timestep) {}));

			SystemDesc parallelB;
			parallelB.Name = "telemetry.test.system.parallelB";
			parallelB.Phase = SystemPhase::Update;
			parallelB.ParallelSafe = true;
			CHECK(registry.Register(parallelB, [](World::Timestep) {}));

			Telemetry::CaptureDesc desc;
			desc.Frames = 1;
			desc.OutputPath = "tmp/telemetry-tests/systems.json";
			CHECK(Telemetry::RequestCapture(desc));

			Telemetry::BeginFrame();
			const uint32_t executed = registry.RunPhase(SystemPhase::Update, World::Timestep(1.0f / 60.0f));
			Telemetry::EndFrame();

			CHECK(executed == 3);
			const Telemetry::CaptureStatus status = Telemetry::GetCaptureStatus();
			CHECK(status.HasResult);
			CHECK(status.DroppedEvents == 0);

			const std::string text = ReadFileText(status.OutputPath);
			CHECK(Contains(text, "telemetry.test.system.serial"));
			CHECK(Contains(text, "telemetry.test.system.parallelA"));
			CHECK(Contains(text, "telemetry.test.system.parallelB"));
			CHECK(CountOccurrences(text, "\"cat\":\"scope\"") >= 3);
			// 系统耗时计数器(每相位一条)。
			CHECK(Contains(text, "systems.executed"));

			World::JobSystem::Shutdown();
		}

		// ---------------------------------------------------------------
		// I. 采集热路径**堆分配 = 0**(方案判据 4 的后半句)。
		//    不是"看起来没分配":用归因表自己的活跃分配数**精确**断言。
		//    为什么可信:本测试模块编译了自己的 GlobalAllocHooks.cpp,所以 MemoryTrack
		//    看得见本进程的每一次 new/delete。
		// ---------------------------------------------------------------
		{
			using namespace World::Profiling;

			Telemetry::CaptureDesc desc;
			// Frames=0 ⇒ 一直采到 CancelCapture。**必须**是 0:若给 1,首个 EndFrame
			// 就会结束采集并释放缓冲,后面的测量全在"采集已关"下进行 ——
			// 那条"零分配"断言会**空过**(实测踩到),这是最危险的假通过。
			desc.Frames = 0;
			desc.OutputPath = "tmp/telemetry-tests/noalloc.json";
			CHECK(Telemetry::RequestCapture(desc));
			CHECK(Telemetry::GetStats().Capture.Active);   // 前提:测量期间采集确实开着

			// 先把一次性开销(缓冲、线程槽位、标签表项)跑完,再看稳态。
			Telemetry::BeginFrame();
			WLD_TRACE_SCOPE("telemetry.test.warmup");
			WLD_TRACE_COUNTER("telemetry.test.warmup", 1);
			Telemetry::EndFrame();

			// 采集必须仍处于开启状态,否则下面的零分配断言无意义。
			CHECK(Telemetry::GetStats().Capture.Active);

			const uint64_t before = MemoryTrack::LiveAllocationCount();
			const uint64_t beforeBytes = MemoryTrack::HeapLiveBytes();

			// 稳态:再跑一帧内的大量作用域与计数器。
			Telemetry::BeginFrame();
			for (int i = 0; i < 2000; ++i)
			{
				WLD_TRACE_SCOPE("telemetry.test.noalloc");
				WLD_TRACE_COUNTER("telemetry.test.noalloc", i);
			}
			Telemetry::EndFrame();

			const uint64_t after = MemoryTrack::LiveAllocationCount();
			const uint64_t afterBytes = MemoryTrack::HeapLiveBytes();

			// 精确断言:采集路径自身既不多分配一次,也不多占一个字节。
			CHECK(after == before);
			CHECK(afterBytes == beforeBytes);

			// 顺带证明这段确实写进了事件(否则"没分配"可能只是没采集)。
			// 用 GetStats():GetCaptureStatus 只在采集**结束**后填数字,采集进行中它还是上一次的结果。
			CHECK(Telemetry::GetStats().Capture.EventCount > 2000);
			Telemetry::CancelCapture();
		}

		Telemetry::Shutdown();
		std::printf("telemetry tests passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "telemetry tests FAILED: %s\n", error.what());
		return 1;
	}
}
