// P2a W6:类型化事件总线 + 计时器。
#include "World/Gameplay/EventBus.h"

#include <cstdio>
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

	struct DamageEvent
	{
		int Amount = 0;
		int Target = 0;
	};

	struct DeathEvent
	{
		int Entity = 0;
	};
}

int main()
{
	try
	{
		using namespace World::Gameplay;

		// 1. 订阅/派发顺序 + 载荷类型安全。
		{
			EventBus bus;
			std::vector<std::string> log;
			const auto damageId = EventBus::TypeIdOf<DamageEvent>();
			CHECK(damageId != EventBus::TypeIdOf<DeathEvent>());

			const auto a = bus.Subscribe<DamageEvent>([&log](const DamageEvent& e)
				{ log.push_back("a:" + std::to_string(e.Amount)); });
			const auto b = bus.Subscribe<DamageEvent>([&log](const DamageEvent& e)
				{ log.push_back("b:" + std::to_string(e.Amount)); });
			const auto death = bus.Subscribe<DeathEvent>([&log](const DeathEvent& e)
				{ log.push_back("death:" + std::to_string(e.Entity)); });
			CHECK(bus.GetSubscriberCount(damageId) == 2);

			bus.Emit(DamageEvent { 7, 3 });
			CHECK(log.size() == 2);
			CHECK(log[0] == "a:7" && log[1] == "b:7");

			bus.Emit(DeathEvent { 42 });
			CHECK(log.size() == 3 && log[2] == "death:42");

			bus.Unsubscribe(b);
			CHECK(bus.GetSubscriberCount(damageId) == 1);
			bus.Emit(DamageEvent { 5, 1 });
			CHECK(log.size() == 4 && log[3] == "a:5");
			(void)a;
			(void)death;
		}

		// 2. 延迟派发:入队不触发,固定点统一派发;派发中取消不崩。
		{
			EventBus bus;
			int hits = 0;
			EventBus::Subscription self;
			self = bus.Subscribe<DamageEvent>([&](const DamageEvent&)
				{
					hits++;
					bus.Unsubscribe(self);   // 派发过程中自取消
				});
			bus.EmitDeferred(DamageEvent { 1, 1 });
			bus.EmitDeferred(DamageEvent { 2, 1 });
			CHECK(hits == 0);
			CHECK(bus.GetPendingCount() == 2);
			CHECK(bus.DispatchPending() == 2);
			CHECK(hits == 1);                 // 自取消后后续事件不再收到
			CHECK(bus.GetSubscriberCount(EventBus::TypeIdOf<DamageEvent>()) == 0);
			CHECK(bus.DispatchPending() == 0);
		}

		// 3. 计时器:After 只触发一次,Every 按次数重复,Cancel 生效。
		{
			TimerService timers;
			int afterHits = 0;
			int everyHits = 0;
			int cancelledHits = 0;
			timers.After(0.5, [&afterHits] { afterHits++; });
			timers.Every(0.25, [&everyHits] { everyHits++; });
			const auto cancelled = timers.Every(0.25, [&cancelledHits] { cancelledHits++; });
			CHECK(timers.GetActiveCount() == 3);

			for (int step = 0; step < 4; ++step)   // 4 * 0.25s = 1.0s
				timers.Advance(0.25);
			CHECK(afterHits == 1);
			CHECK(everyHits == 4);
			CHECK(cancelledHits == 4);

			timers.Cancel(cancelled);
			timers.Advance(0.25);
			CHECK(cancelledHits == 4);          // 取消后不再触发
			CHECK(everyHits == 5);

			// 限次数:每 0.1s 一次、共 2 次。
			int limitedHits = 0;
			timers.Every(0.1, [&limitedHits] { limitedHits++; }, 2);
			for (int step = 0; step < 6; ++step)
				timers.Advance(0.1);
			CHECK(limitedHits == 2);

			timers.Clear();
			CHECK(timers.GetActiveCount() == 0);
		}

		std::printf("World.Events: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Events FAILED: %s\n", error.what());
		return 1;
	}
}
