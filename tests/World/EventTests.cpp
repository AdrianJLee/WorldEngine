// P2a W6 + P2 W4:类型化事件总线 + 计时器(含派发隔离与 GameApp 接线契约)。
//
// 覆盖:
//   1. 订阅/派发顺序与载荷类型安全;
//   2. 延迟派发:入队不触发、固定点统一派发、派发中取消不崩;
//   3. 计时器:After/Every/Cancel 基本语义;
//   4. 派发隔离:handler 抛异常不逃出、不丢本批剩余事件,总线在异常后仍可用(退订立即生效);
//   5. 派发中 emit(DeferredRaw)→ 留到下一批,不同步重入;
//   6. 计时器回调内的 After/Cancel/Clear 安全 + 回调异常隔离 + 60Hz 步数精确(误差 < 1 步);
//   7. GameApp 接线:固定步内 Timers.Advance 先于 FixedUpdate/系统,帧末 DispatchPending
//      在 Update 之后;暂停时计时器冻结但事件仍投递。
#include "World/Core/Timestep.h"
#include "World/Gameplay/EventBus.h"
#include "World/Gameplay/GameApp.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <stdexcept>
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
		using namespace World;
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

		// 4. 派发隔离:handler 抛异常不逃出,也不丢本批剩余事件/后续订阅者。
		{
			EventBus bus;
			int delivered = 0;
			const auto throwing = bus.Subscribe<DamageEvent>([](const DamageEvent&)
				{ throw std::runtime_error("handler exploded"); });
			const auto counting = bus.Subscribe<DamageEvent>([&delivered](const DamageEvent&) { ++delivered; });

			// 立即派发:EmitRaw 不让异常逃出。
			bus.Emit(DamageEvent { 1, 0 });
			CHECK(delivered == 1);
			CHECK(bus.GetHandlerErrorCount() == 1);
			CHECK(!bus.GetLastHandlerError().empty());

			// 延迟派发:一批两条事件,异常不得让第二条丢失。
			bus.EmitDeferred(DamageEvent { 2, 0 });
			bus.EmitDeferred(DamageEvent { 3, 0 });
			CHECK(bus.DispatchPending() == 2);
			CHECK(delivered == 3);
			CHECK(bus.GetHandlerErrorCount() == 3);

			// 异常之后总线仍可用:退订立即生效(证明 m_Dispatching 已被 RAII 复位)。
			bus.Unsubscribe(throwing);
			CHECK(bus.GetSubscriberCount(EventBus::TypeIdOf<DamageEvent>()) == 1);
			bus.Emit(DamageEvent { 4, 0 });
			CHECK(delivered == 4);
			CHECK(bus.GetHandlerErrorCount() == 3);
			(void)counting;
		}

		// 5. 派发中 emit(队列式)→ 留到下一批,不在当前批次里同步重入。
		{
			EventBus bus;
			std::vector<std::string> log;
			bus.Subscribe<DamageEvent>([&](const DamageEvent& e)
				{
					log.push_back("a:" + std::to_string(e.Amount));
					if (e.Amount == 1)
						bus.EmitDeferred(DamageEvent { 2, 0 });   // 派发中 emit
				});
			bus.Subscribe<DamageEvent>([&](const DamageEvent& e)
				{ log.push_back("b:" + std::to_string(e.Amount)); });

			bus.EmitDeferred(DamageEvent { 1, 0 });
			CHECK(bus.DispatchPending() == 1);
			CHECK(log.size() == 2 && log[0] == "a:1" && log[1] == "b:1");
			CHECK(bus.GetPendingCount() == 1);      // 新事件留到下一批
			CHECK(bus.DispatchPending() == 1);
			CHECK(log.size() == 4 && log[2] == "a:2" && log[3] == "b:2");
			CHECK(bus.DispatchPending() == 0);

			// 派发中新增订阅(会让 m_Entries 重新分配):不崩,新订阅从下一条事件起生效。
			int lateHits = 0;
			EventBus::Subscription late;
			bus.Subscribe<DeathEvent>([&](const DeathEvent&)
				{
					if (!late.IsValid())
						late = bus.Subscribe<DeathEvent>([&lateHits](const DeathEvent&) { ++lateHits; });
				});
			bus.EmitDeferred(DeathEvent { 1 });
			CHECK(bus.DispatchPending() == 1);
			CHECK(lateHits == 0);                 // 派发中新增的订阅不收到本条(既有契约)
			bus.EmitDeferred(DeathEvent { 2 });
			CHECK(bus.DispatchPending() == 1);
			CHECK(lateHits == 1);
		}

		// 6. 计时器回调内的结构变更安全 + 回调异常隔离。
		{
			TimerService timers;
			std::vector<std::string> log;
			TimerService::Handle self;
			self = timers.Every(0.1, [&]
				{
					log.push_back("every");
					timers.After(0.1, [&] { log.push_back("nested"); });   // 回调内新增
					timers.Cancel(self);                                   // 回调内取消自己
				});
			CHECK(timers.Advance(0.1) == 1);
			CHECK(log.size() == 1 && log[0] == "every");
			CHECK(timers.GetActiveCount() == 1);    // self 已取消,nested 从下一步开始
			timers.Advance(0.1);
			CHECK(log.size() == 2 && log[1] == "nested");
			CHECK(timers.Advance(0.1) == 0);

			// Clear 在回调内:本步后续到期条目不再触发。
			TimerService clearing;
			int hits = 0;
			clearing.Every(0.1, [&] { ++hits; clearing.Clear(); });
			clearing.Every(0.1, [&] { hits += 100; });
			CHECK(clearing.Advance(0.1) == 1);
			CHECK(hits == 1);
			CHECK(clearing.GetActiveCount() == 0);
			CHECK(clearing.Advance(0.1) == 0);

			// 回调异常:被记录、不逃出,后续条目继续触发。
			TimerService throwingTimers;
			int later = 0;
			throwingTimers.After(0.1, [] { throw std::runtime_error("timer exploded"); });
			throwingTimers.After(0.1, [&later] { ++later; });
			CHECK(throwingTimers.Advance(0.1) == 2);
			CHECK(later == 1);
			CHECK(throwingTimers.GetCallbackErrorCount() == 1);
			CHECK(!throwingTimers.GetLastCallbackError().empty());
		}

		// 7. 60Hz 触发步数精确:0.5s → 第 30/60/90/120 步,1.0s → 第 60 步(误差 < 1 步)。
		{
			constexpr double kStep = 1.0 / 60.0;
			TimerService timers;
			std::vector<int> everyFires;
			int afterFires = 0;
			int afterStep = -1;
			int step = 0;
			timers.Every(0.5, [&] { everyFires.push_back(step); });
			timers.After(1.0, [&] { ++afterFires; if (afterStep < 0) afterStep = step; });
			for (step = 1; step <= 121; ++step)
				timers.Advance(kStep);

			CHECK(afterFires == 1);
			CHECK(std::fabs(static_cast<double>(afterStep - 60)) < 1.0);
			CHECK(everyFires.size() == 4);
			for (std::size_t index = 0; index < everyFires.size(); ++index)
			{
				const double expected = 30.0 * static_cast<double>(index + 1);
				CHECK(std::fabs(static_cast<double>(everyFires[index]) - expected) < 1.0);
			}

			TimerService thirds;
			std::vector<int> thirdFires;
			step = 0;
			thirds.Every(1.0 / 3.0, [&] { thirdFires.push_back(step); });
			for (step = 1; step <= 60; ++step)
				thirds.Advance(kStep);
			CHECK(thirdFires.size() == 3);   // 20/40/60 步
			CHECK(std::fabs(static_cast<double>(thirdFires[0]) - 20.0) < 1.0);
		}

		// 8. GameApp 接线:Timers.Advance 在固定步内先于 FixedUpdate;DispatchPending 在帧末;
		//    暂停时计时器冻结但事件仍投递。
		{
			GameApp::Shutdown();
			GameAppDesc desc;
			desc.ProjectId = "events-tick-wiring-test";
			desc.FixedStepHz = 60;
			GameApp::Create(desc);
			GameApp& app = GameApp::Get();

			std::vector<std::string> order;
			app.Events().Subscribe<DamageEvent>([&order](const DamageEvent&)
				{ order.push_back("event"); });
			app.SetPhaseCallbacks(
				[&order](Timestep) { order.push_back("fixed"); },
				[&order](Timestep) { order.push_back("update"); },
				nullptr);
			app.Timers().After(app.FixedStepSeconds(), [&order] { order.push_back("timer"); });
			app.Events().EmitDeferred(DamageEvent { 1, 0 });
			app.Tick(Timestep(static_cast<float>(app.FixedStepSeconds())));
			CHECK(order.size() == 4);
			CHECK(order[0] == "timer");     // 固定步内:计时器先于 FixedUpdate
			CHECK(order[1] == "fixed");
			CHECK(order[2] == "update");
			CHECK(order[3] == "event");     // 帧末:Update 之后才派发

			// 暂停:固定步与可变阶段都停(计时器不推进),但帧末仍投递事件。
			int pausedTimerHits = 0;
			int pausedEventHits = 0;
			app.Timers().After(app.FixedStepSeconds(), [&pausedTimerHits] { ++pausedTimerHits; });
			app.Events().Subscribe<DeathEvent>([&pausedEventHits](const DeathEvent&) { ++pausedEventHits; });
			app.SetPaused(true);
			app.Events().EmitDeferred(DeathEvent { 7 });
			app.Tick(Timestep(static_cast<float>(app.FixedStepSeconds())));
			CHECK(app.LastFixedSteps() == 0);
			CHECK(pausedTimerHits == 0);
			CHECK(pausedEventHits == 1);

			// 恢复:积压被截断为一个步长,计时器恰好触发一次。
			app.SetPaused(false);
			app.Tick(Timestep(static_cast<float>(app.FixedStepSeconds())));
			CHECK(pausedTimerHits == 1);
			CHECK(!app.LastFramePhases().empty());
			GameApp::Shutdown();
			CHECK(!GameApp::Exists());
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
