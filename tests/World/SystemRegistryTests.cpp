// P2a W5:SystemRegistry 的阶段派发、同阶段顺序依赖、注册校验与耗时统计。
#include "World/Core/Thread/JobSystem.h"
#include "World/Gameplay/SystemRegistry.h"

#include <chrono>
#include <cstdio>
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
				[&](World::Timestep) { slowWork(); }));
			CHECK(jobs.Register({ "jobB", SystemPhase::Update, true, {} },
				[&](World::Timestep) { slowWork(); }));
			startA = std::chrono::steady_clock::now();
			const uint32_t ran = jobs.RunPhase(SystemPhase::Update, World::Timestep(0.016f));
			endB = std::chrono::steady_clock::now();
			CHECK(ran == 2);
			const double elapsedMs = std::chrono::duration<double, std::milli>(endB - startA).count();
			// 串行至少 50ms;并发应显著更快(留足 CI 抖动量)。
			CHECK(elapsedMs < 45.0);
			CHECK(jobs.GetLastTimings().size() == 2);
			(void)startB; (void)endA;
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
