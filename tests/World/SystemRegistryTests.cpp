// P2a W5:SystemRegistry 的阶段派发、同阶段顺序依赖、注册校验与耗时统计。
#include "World/Core/Thread/JobSystem.h"
#include "World/Gameplay/Runtime/SystemRegistry.h"

#include <chrono>
#include <cstdio>
#include <atomic>
#include <thread>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)
}

int main()
{
	try
	{
		using namespace World::Gameplay;
		// 并行用例需要任务系统(未初始化时 RunPhase 会退化为串行)。
		World::JobSystem::Init();
		CHECK(World::JobSystem::IsRunning());

		SystemRegistry registry;
		std::vector<std::string> log;

		// 1. 注册校验:空名/重复/非法阶段/未知依赖/自依赖都被拒绝。
		{
			CHECK(!registry.Register({ "", SystemPhase::Update, false, {} }, [](World::Timestep) {}));
			CHECK(!registry.Register({ "bad", static_cast<SystemPhase>(99), false, {} }, [](World::Timestep) {}));
			CHECK(registry.Register({ "physics", SystemPhase::Fixed, true, {} },
				[&log](World::Timestep) { log.push_back("physics"); }));
			CHECK(!registry.Register({ "physics", SystemPhase::Fixed, true, {} }, [](World::Timestep) {}));
			// 前向引用允许:依赖是否成立在 RunPhase 判定(见用例 3)。
			CHECK(!registry.Register({ "self", SystemPhase::Fixed, false, { "self" } }, [](World::Timestep) {}));
			CHECK(registry.GetSystemCount() == 1);
			CHECK(!registry.GetLastError().empty());
		}

		// 2. 阶段派发顺序:PreFixed -> Fixed -> Update -> Late;每阶段只跑自己的系统。
		{
			CHECK(registry.Register({ "pre", SystemPhase::PreFixed, false, {} },
				[&log](World::Timestep) { log.push_back("pre"); }));
			CHECK(registry.Register({ "ai", SystemPhase::Update, false, { "physics" } },
				[&log](World::Timestep) { log.push_back("ai"); }));
			CHECK(registry.Register({ "camera", SystemPhase::Late, false, {} },
				[&log](World::Timestep) { log.push_back("camera"); }));

			CHECK(registry.RunPhase(SystemPhase::PreFixed, World::Timestep(0.016f)) == 1);
			CHECK(registry.RunPhase(SystemPhase::Fixed, World::Timestep(0.016f)) == 1);
			CHECK(registry.RunPhase(SystemPhase::Update, World::Timestep(0.016f)) == 1);
			CHECK(registry.RunPhase(SystemPhase::Late, World::Timestep(0.016f)) == 1);
			CHECK(registry.RunPhase(SystemPhase::PreRender, World::Timestep(0.016f)) == 0);

			const std::vector<std::string> expected { "pre", "physics", "ai", "camera" };
			CHECK(log == expected);
		}

		// 3. 同阶段顺序依赖:声明 After 的系统排在目标之后(注册顺序相反也要纠正)。
		{
			SystemRegistry ordered;
			std::vector<std::string> order;
			CHECK(ordered.Register({ "second", SystemPhase::Update, false, { "first" } },
				[&order](World::Timestep) { order.push_back("second"); }));
			CHECK(ordered.Register({ "first", SystemPhase::Update, false, {} },
				[&order](World::Timestep) { order.push_back("first"); }));
			CHECK(ordered.RunPhase(SystemPhase::Update, World::Timestep(0.016f)) == 2);
			CHECK(order.size() == 2 && order[0] == "first" && order[1] == "second");
		}

		// 4. 耗时与运行计数:每次 RunPhase 记录逐系统耗时,非负且名字/阶段/并行标记正确。
		{
			CHECK(registry.RunPhase(SystemPhase::Late, World::Timestep(0.016f)) == 1);   // 重新跑一次 Late 以便读耗时
			const auto& timings = registry.GetLastTimings();
			CHECK(timings.size() == 1);
			CHECK(timings[0].Name == "camera");
			CHECK(timings[0].Phase == SystemPhase::Late);
			CHECK(timings[0].Milliseconds >= 0.0);
			CHECK(registry.GetRunCount() == 5);
		}

		// 5. 注销:被依赖的系统不能先注销,解除依赖后可以。
		{
			CHECK(!registry.Unregister("physics"));   // ai 依赖它
			CHECK(registry.Unregister("ai"));
			CHECK(registry.Unregister("physics"));
			CHECK(registry.GetSystemCount() == 2);   // 剩 pre 与 camera
			registry.Clear();
			CHECK(registry.GetSystemCount() == 0);
		}

		// 6. 并行安全系统:两个 ParallelSafe 系统应真并发(执行时间区间重叠),串行系统仍按序。
		{
			SystemRegistry jobs;
			std::chrono::steady_clock::time_point startA, endA, startB, endB;
			const auto slowWork = [] { std::this_thread::sleep_for(std::chrono::milliseconds(25)); };
			CHECK(jobs.Register({ "jobA", SystemPhase::Update, true, {} },
				[&](World::Timestep)
				{
					startA = std::chrono::steady_clock::now();
					slowWork();
					endA = std::chrono::steady_clock::now();
				}));
			CHECK(jobs.Register({ "jobB", SystemPhase::Update, true, {} },
				[&](World::Timestep)
				{
					startB = std::chrono::steady_clock::now();
					slowWork();
					endB = std::chrono::steady_clock::now();
				}));
			// PROJ-6:抖动修复 —— 原来只测一次总耗时 < 45ms(两作业各睡 25ms:串行 50ms、并发 ~25ms)。
			// 刚并行重链后的负载窗口里单次测量会偶发超过 45ms,与"是否并发"无关(实测复现 1/57)。
			// 改成两条判据:①语义 = 两个作业的执行区间必须重叠(串行必然不重叠);
			//             ②时间 = 最多测 3 次取最快(< 45ms;串行下三次都 >= 50ms,不会误判)。
			uint32_t ran = 0;
			double bestElapsedMs = 1.0e9;
			bool overlapped = false;
			for (int attempt = 0; attempt < 3; ++attempt)
			{
				const auto phaseStart = std::chrono::steady_clock::now();
				ran = jobs.RunPhase(SystemPhase::Update, World::Timestep(0.016f));
				const double elapsedMs = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - phaseStart).count();
				bestElapsedMs = elapsedMs < bestElapsedMs ? elapsedMs : bestElapsedMs;
				overlapped = overlapped || (startA < endB && startB < endA);
			}
			CHECK(ran == 2);
			CHECK(overlapped);
			CHECK(bestElapsedMs < 45.0);
			CHECK(jobs.GetLastTimings().size() == 2);
			std::printf("World.Systems: parallel phase best of 3 = %.2f ms (overlap=%s)\n",
				bestElapsedMs, overlapped ? "true" : "false");
		}
				// 6. 定时间隔节流:设置 Interval > 0 时按周期推进,未达间隔直接跳过。
		{
			SystemRegistry throttled;
			int runCount = 0;
			float totalAccumulatedDt = 0.0f;
			SystemDesc desc;
			desc.Name = "throttled";
			desc.Phase = SystemPhase::Update;
			desc.Interval = 0.2f; // 每 0.2s 推进一次 (5Hz)
			CHECK(throttled.Register(desc, [&runCount, &totalAccumulatedDt](World::Timestep dt) {
				++runCount;
				totalAccumulatedDt += dt.GetSeconds();
			}));

			// 模拟 1.0 秒,单步 dt = 0.05s (共 20 步)
			for (int step = 0; step < 20; ++step)
			{
				throttled.RunPhase(SystemPhase::Update, World::Timestep(0.05f));
			}
			// 1.0 秒 / 0.2 秒 = 精确执行 5 次!
			CHECK(runCount == 5);
			CHECK(std::abs(totalAccumulatedDt - 1.0f) < 0.01f);
		}

		// 7. 条件门禁:Condition 返回 false 时本轮直接跳过,返回 true 时正常执行。
		{
			SystemRegistry gated;
			bool allowRun = false;
			int executionCount = 0;
			SystemDesc desc;
			desc.Name = "gated";
			desc.Phase = SystemPhase::Update;
			desc.Condition = [&allowRun]() { return allowRun; };
			CHECK(gated.Register(desc, [&executionCount](World::Timestep) {
				++executionCount;
			}));

			CHECK(gated.RunPhase(SystemPhase::Update, World::Timestep(0.016f)) == 0);
			CHECK(executionCount == 0);

			allowRun = true;
			CHECK(gated.RunPhase(SystemPhase::Update, World::Timestep(0.016f)) == 1);
			CHECK(executionCount == 1);

			allowRun = false;
			CHECK(gated.RunPhase(SystemPhase::Update, World::Timestep(0.016f)) == 0);
			CHECK(executionCount == 1);
		}

		// ---- WP4 声明式并行调度:并发/保序/保守三条反假通过用例 ----

		// 等待另一个系统同时进入(有上限,避免串行时死等)。返回是否观察到重叠。
		const auto awaitOverlap = [](std::atomic<int>& inFlight)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
			while (inFlight.load(std::memory_order_acquire) < 2)
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		};
		const auto probeOverlap = [&awaitOverlap](std::atomic<int>& inFlight, std::atomic<bool>& overlapped)
		{
			if (awaitOverlap(inFlight))
				overlapped.store(true);
		};

		// 8. A 并行:两个声明了不相交读写集的系统必须真并发(区间重叠,而非"结果正确")。
		{
			SystemRegistry dag;
			constexpr entt::id_type kShared = 100;
			constexpr entt::id_type kWriteA = 101;
			constexpr entt::id_type kWriteB = 102;
			std::atomic<int> active { 0 };
			std::atomic<bool> overlapped { false };

			SystemDesc a;
			a.Name = "decl-a";
			a.Phase = SystemPhase::Update;
			a.Reads = { kShared };
			a.Writes = { kWriteA };
			a.AccessDeclared = true;
			SystemDesc b;
			b.Name = "decl-b";
			b.Phase = SystemPhase::Update;
			b.Reads = { kShared };
			b.Writes = { kWriteB };
			b.AccessDeclared = true;

			int ranA = 0, ranB = 0;
			CHECK(dag.Register(a, [&](World::Timestep)
			{
				active.fetch_add(1, std::memory_order_acq_rel);
				probeOverlap(active, overlapped);
				++ranA;
				active.fetch_sub(1, std::memory_order_acq_rel);
			}));
			CHECK(dag.Register(b, [&](World::Timestep)
			{
				active.fetch_add(1, std::memory_order_acq_rel);
				probeOverlap(active, overlapped);
				++ranB;
				active.fetch_sub(1, std::memory_order_acq_rel);
			}));
			CHECK(dag.RunPhase(SystemPhase::Update, World::Timestep(0.016f)) == 2);
			CHECK(ranA == 1 && ranB == 1);
			CHECK(overlapped.load());                       // 串行执行时永远观察不到重叠
			CHECK(dag.GetLastTimings().size() == 2);
		}

		// 9. B 保序:读写同一组件(写-读冲突)必须严格串行且按注册序(与名字字典序无关)。
		{
			SystemRegistry conflicting;
			constexpr entt::id_type kComponent = 200;
			std::atomic<int> active { 0 };
			std::atomic<bool> overlapped { false };
			std::vector<std::string> sequence;

			SystemDesc writer;
			writer.Name = "zzz-writer";      // 名字靠后,但先注册
			writer.Phase = SystemPhase::Update;
			writer.Writes = { kComponent };
			writer.AccessDeclared = true;
			SystemDesc reader;
			reader.Name = "aaa-reader";      // 名字靠前,但后注册
			reader.Phase = SystemPhase::Update;
			reader.Reads = { kComponent };
			reader.AccessDeclared = true;

			CHECK(conflicting.Register(writer, [&](World::Timestep)
			{
				active.fetch_add(1, std::memory_order_acq_rel);
				probeOverlap(active, overlapped);
				sequence.push_back("zzz-writer");
				active.fetch_sub(1, std::memory_order_acq_rel);
			}));
			CHECK(conflicting.Register(reader, [&](World::Timestep)
			{
				active.fetch_add(1, std::memory_order_acq_rel);
				probeOverlap(active, overlapped);
				sequence.push_back("aaa-reader");
				active.fetch_sub(1, std::memory_order_acq_rel);
			}));
			CHECK(conflicting.RunPhase(SystemPhase::Update, World::Timestep(0.016f)) == 2);
			CHECK(!overlapped.load());
			CHECK(sequence.size() == 2 && sequence[0] == "zzz-writer" && sequence[1] == "aaa-reader");
		}

		// 10. C 未声明保守:未声明(且未 ParallelSafe)+ 已声明 ⇒ 保守串行,不并发。
		{
			SystemRegistry conservative;
			constexpr entt::id_type kComp = 300;
			std::atomic<int> active { 0 };
			std::atomic<bool> overlapped { false };

			SystemDesc declared;
			declared.Name = "declared";
			declared.Phase = SystemPhase::Update;
			declared.Reads = { kComp };
			declared.AccessDeclared = true;
			SystemDesc legacy;
			legacy.Name = "legacy-undeclared";
			legacy.Phase = SystemPhase::Update;   // ParallelSafe=false 且未声明

			CHECK(conservative.Register(declared, [&](World::Timestep)
			{
				active.fetch_add(1, std::memory_order_acq_rel);
				probeOverlap(active, overlapped);
				active.fetch_sub(1, std::memory_order_acq_rel);
			}));
			CHECK(conservative.Register(legacy, [&](World::Timestep)
			{
				active.fetch_add(1, std::memory_order_acq_rel);
				probeOverlap(active, overlapped);
				active.fetch_sub(1, std::memory_order_acq_rel);
			}));
			CHECK(conservative.RunPhase(SystemPhase::Update, World::Timestep(0.016f)) == 2);
			CHECK(!overlapped.load());
		}

		// 11. 冲突判定细则:同组件"读-读"不算冲突(可并发);"写-写"冲突(串行)。
		{
			constexpr entt::id_type kReadOnly = 400;

			SystemRegistry readRead;
			std::atomic<int> activeRR { 0 };
			std::atomic<bool> overlappedRR { false };
			SystemDesc r1;
			r1.Name = "read-1";
			r1.Phase = SystemPhase::Update;
			r1.Reads = { kReadOnly };
			r1.AccessDeclared = true;
			SystemDesc r2 = r1;
			r2.Name = "read-2";
			CHECK(readRead.Register(r1, [&](World::Timestep)
			{
				activeRR.fetch_add(1, std::memory_order_acq_rel);
				probeOverlap(activeRR, overlappedRR);
				activeRR.fetch_sub(1, std::memory_order_acq_rel);
			}));
			CHECK(readRead.Register(r2, [&](World::Timestep)
			{
				activeRR.fetch_add(1, std::memory_order_acq_rel);
				probeOverlap(activeRR, overlappedRR);
				activeRR.fetch_sub(1, std::memory_order_acq_rel);
			}));
			CHECK(readRead.RunPhase(SystemPhase::Update, World::Timestep(0.016f)) == 2);
			CHECK(overlappedRR.load());

			SystemRegistry writeWrite;
			std::atomic<int> activeWW { 0 };
			std::atomic<bool> overlappedWW { false };
			SystemDesc w1;
			w1.Name = "write-1";
			w1.Phase = SystemPhase::Update;
			w1.Writes = { kReadOnly };
			w1.AccessDeclared = true;
			SystemDesc w2 = w1;
			w2.Name = "write-2";
			CHECK(writeWrite.Register(w1, [&](World::Timestep)
			{
				activeWW.fetch_add(1, std::memory_order_acq_rel);
				probeOverlap(activeWW, overlappedWW);
				activeWW.fetch_sub(1, std::memory_order_acq_rel);
			}));
			CHECK(writeWrite.Register(w2, [&](World::Timestep)
			{
				activeWW.fetch_add(1, std::memory_order_acq_rel);
				probeOverlap(activeWW, overlappedWW);
				activeWW.fetch_sub(1, std::memory_order_acq_rel);
			}));
			CHECK(writeWrite.RunPhase(SystemPhase::Update, World::Timestep(0.016f)) == 2);
			CHECK(!overlappedWW.load());
		}

		World::JobSystem::Shutdown();
		std::printf("World.Systems: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Systems FAILED: %s\n", error.what());
		return 1;
	}
}
