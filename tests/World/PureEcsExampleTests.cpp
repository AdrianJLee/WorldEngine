// Pure ECS M6: 端到端纯 ECS 游戏性示例套件 (Pure ECS End-to-End Gameplay Example Suite)
//
// 本套件全面展示并验收 WorldEngine 纯数据驱动 ECS (Pure ECS) 体系：
//   1. 纯 POD 数据组件 (Position2D, Velocity2D, Health, DamageEvent) 与零字节状态 Tag 组件 (HeroTag, MonsterTag, DeadTag)；
//   2. 派生 World::ISystem 的 C++ 纯系统 (MovementSystem, DamageProcessingSystem)；
//   3. 现代 Query DSL 链式过滤与批量迭代 (scene.Query<...>().Without<...>().Each(...))；
//   4. 实体流式装配命令 (scene.CreateEntityShell(...).Set<...>().With<...>().Without<...>())；
//   5. 响应式组件观察者 (scene.OnAdd<DeadTag>(...)) 捕获实体状态变更事件；
//   6. Luau 纯 ECS 系统与 C++ 系统在同一帧管线中的并行驱动与协同更新。

#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/ISystem.h"
#include "World/Scene/Query.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/LuauVm.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
	using namespace World;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(...) Check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __LINE__)

	bool ApproxEqual(float a, float b, float eps = 0.001f)
	{
		return std::fabs(a - b) <= eps;
	}

	// =========================================================================
	// 1. 纯 POD 数据组件与零字节状态 Tag 组件
	// =========================================================================
	struct Position2D
	{
		float X = 0.0f;
		float Y = 0.0f;
	};

	struct Velocity2D
	{
		float Vx = 0.0f;
		float Vy = 0.0f;
	};

	struct Health
	{
		float Current = 100.0f;
		float Max = 100.0f;
	};

	struct DamageEvent
	{
		float Amount = 0.0f;
	};

	struct HeroTag {};
	struct MonsterTag {};
	struct DeadTag {};

	// =========================================================================
	// 2. C++ 纯 ECS 系统 (派生 World::ISystem)
	// =========================================================================

	// 战斗伤害结算系统: 扫描 Health 与 DamageEvent，扣减生命值，扣至 0 贴上 DeadTag 并移除 DamageEvent
	class DamageProcessingSystem : public World::ISystem
	{
	public:
		std::string_view Name() const override { return "DamageProcessingSystem"; }

		void Update(World::Scene& scene, World::Timestep dt) override
		{
			(void)dt;
			std::vector<entt::entity> deadEntities;
			std::vector<entt::entity> processedEntities;

			// 使用 Query DSL 批量遍历所有携带 Health 和 DamageEvent 的实体
			scene.Query<Health, DamageEvent>()
				.Each([&deadEntities, &processedEntities](entt::entity e, Health& health, const DamageEvent& dmg) {
					health.Current -= dmg.Amount;
					if (health.Current <= 0.0f)
					{
						health.Current = 0.0f;
						deadEntities.push_back(e);
					}
					processedEntities.push_back(e);
				});

			// 为生命值归零的实体打上 DeadTag 并触发 OnAdd 响应式观察者
			for (entt::entity e : deadEntities)
			{
				Entity entity(&scene, e);
				entity.Set<DeadTag>();
				scene.NotifyComponentAdded(entity, entt::type_id<DeadTag>().hash());
			}

			// 结算完毕后移除一次性 DamageEvent 暂态组件
			for (entt::entity e : processedEntities)
			{
				Entity entity(&scene, e);
				entity.Without<DamageEvent>();
			}
		}
	};

	// 空间位移系统: 扫描 Position2D 与 const Velocity2D，排除已死亡 (Without<DeadTag>) 的实体
	class MovementSystem : public World::ISystem
	{
	public:
		std::string_view Name() const override { return "MovementSystem"; }

		void Update(World::Scene& scene, World::Timestep dt) override
		{
			const float delta = dt.GetSeconds();
			// 链式 Query DSL: 批量推进存活实体位置
			scene.Query<Position2D, const Velocity2D>()
				.Without<DeadTag>()
				.Each([delta](Position2D& pos, const Velocity2D& vel) {
					pos.X += vel.Vx * delta;
					pos.Y += vel.Vy * delta;
				});
		}
	};
}

int main()
{
	try
	{
		using namespace World;

		// -----------------------------------------------------------------
		// 校验 1: 组件 POD 与 Tag 零开销静态特征断言
		// -----------------------------------------------------------------
		static_assert(std::is_standard_layout_v<Position2D>, "Position2D must be standard layout");
		static_assert(std::is_standard_layout_v<Velocity2D>, "Velocity2D must be standard layout");
		static_assert(std::is_standard_layout_v<Health>, "Health must be standard layout");
		static_assert(std::is_standard_layout_v<DamageEvent>, "DamageEvent must be standard layout");
		static_assert(std::is_empty_v<HeroTag>, "HeroTag must be a zero-byte tag");
		static_assert(std::is_empty_v<MonsterTag>, "MonsterTag must be a zero-byte tag");
		static_assert(std::is_empty_v<DeadTag>, "DeadTag must be a zero-byte tag");

		// 初始化核心上下文与脚本引擎
		ScriptEngine::Init();
		WorldContext context;
		Scene scene(context);
		ScriptEngine::SetActiveScene(&scene);

		// -----------------------------------------------------------------
		// 校验 2: 响应式组件观察者注册 (scene.OnAdd<DeadTag>)
		// -----------------------------------------------------------------
		std::vector<Entity> deadEntityLog;
		uint64_t deathObserverHandle = scene.OnAdd<DeadTag>([&deadEntityLog](Entity deadEntity) {
			deadEntityLog.push_back(deadEntity);
		});
		CHECK(deathObserverHandle > 0);

		// -----------------------------------------------------------------
		// 校验 3: 流式实体装配 (scene.CreateEntityShell(...).Set<...>().With<...>())
		// -----------------------------------------------------------------
		// 英雄实体 (Hero)
		Entity hero = scene.CreateEntityShell("Hero")
			.Set<Position2D>(0.0f, 0.0f)
			.With<Velocity2D>(10.0f, 0.0f)
			.With<Health>(200.0f, 200.0f)
			.With<HeroTag>();

		// 增加 TransformComponent 用于验证与 Luau 系统跨语言管线协同
		auto& heroTransform = hero.AddComponent<TransformComponent>();
		heroTransform.Location = glm::vec3(0.0f, 0.0f, 0.0f);

		// 怪物 1: 哥布林 (Goblin, 低生命)
		Entity goblin = scene.CreateEntityShell("Goblin")
			.Set<Position2D>(10.0f, 0.0f)
			.With<Velocity2D>(-2.0f, 0.0f)
			.With<Health>(30.0f, 30.0f)
			.With<MonsterTag>();

		// 怪物 2: 兽人 (Orc, 高生命)
		Entity orc = scene.CreateEntityShell("Orc")
			.Set<Position2D>(25.0f, 10.0f)
			.With<Velocity2D>(0.0f, -1.0f)
			.With<Health>(150.0f, 150.0f)
			.With<MonsterTag>();

		CHECK(hero.IsValid());
		CHECK(goblin.IsValid());
		CHECK(orc.IsValid());
		CHECK(hero.HasComponent<HeroTag>());
		CHECK(goblin.HasComponent<MonsterTag>());
		CHECK(orc.HasComponent<MonsterTag>());
		CHECK(!goblin.HasComponent<DeadTag>());

		// -----------------------------------------------------------------
		// 校验 4: C++ 纯系统与 Luau 纯 ECS 系统在同一帧管线中的并行注册与驱动
		// -----------------------------------------------------------------
		// 注册 C++ 系统 (按伤害结算 -> 空间位移顺序挂载)
		auto& dmgSys = scene.RegisterSystem<DamageProcessingSystem>();
		auto& moveSys = scene.RegisterSystem<MovementSystem>();
		CHECK(dmgSys.Name() == "DamageProcessingSystem");
		CHECK(moveSys.Name() == "MovementSystem");

		// 注册 Luau 纯 ECS 系统: 协同驱动 TransformComponent
		std::string runErr;
		bool luauOk = ScriptEngine::GetState().RunString(R"(
			ecs:AddSystem("LuauCombatEffectSystem", "Update", function(dt)
				local q = ecs:Query({"TransformComponent"})
				q:Each(function(e, t)
					t.Location = t.Location + vec3.new(0.0, 5.0 * dt, 0.0)
				end)
			end)
		)", "LuauCombatSystem", &runErr);
		CHECK(luauOk);

		// -----------------------------------------------------------------
		// 校验 5: 端到端 3 帧模拟更新与状态迁移验证
		// -----------------------------------------------------------------

		// === 模拟第 1 帧: 正常移动与非致命伤害结算 (dt = 1.0s) ===
		goblin.Set<DamageEvent>(10.0f); // 哥布林承受 10 点伤害 (剩余 20)
		orc.Set<DamageEvent>(25.0f);    // 兽人承受 25 点伤害 (剩余 125)

		scene.OnUpdateRuntime(Timestep(1.0f));

		// 验证伤害结算结果
		CHECK(ApproxEqual(goblin.GetComponent<Health>().Current, 20.0f));
		CHECK(ApproxEqual(orc.GetComponent<Health>().Current, 125.0f));
		CHECK(!goblin.HasComponent<DamageEvent>()); // DamageEvent 暂态已被移除
		CHECK(!orc.HasComponent<DamageEvent>());
		CHECK(!goblin.HasComponent<DeadTag>());     // 尚未死亡
		CHECK(deadEntityLog.empty());               // 死亡观察者未触发

		// 验证位置更新 (MovementSystem)
		CHECK(ApproxEqual(hero.GetComponent<Position2D>().X, 10.0f));
		CHECK(ApproxEqual(goblin.GetComponent<Position2D>().X, 8.0f));
		CHECK(ApproxEqual(orc.GetComponent<Position2D>().Y, 9.0f));

		// 验证 Luau 系统同一帧调度效果 (Y 轴累加 5.0 * 1.0 = 5.0)
		CHECK(ApproxEqual(hero.GetComponent<TransformComponent>().Location.y, 5.0f));

		// === 模拟第 2 帧: 致命伤害结算、DeadTag 贴标与响应式死亡观察者触发 (dt = 1.0s) ===
		goblin.Set<DamageEvent>(50.0f); // 致命伤害: 剩余 20 - 50 <= 0 -> 死亡!
		orc.Set<DamageEvent>(15.0f);    // 兽人承受 15 点伤害 (剩余 110)

		scene.OnUpdateRuntime(Timestep(1.0f));

		// 验证哥布林生命值归零、贴上 DeadTag、死亡观察者捕获
		CHECK(ApproxEqual(goblin.GetComponent<Health>().Current, 0.0f));
		CHECK(goblin.HasComponent<DeadTag>());
		CHECK(!goblin.HasComponent<DamageEvent>());
		CHECK(deadEntityLog.size() == 1);
		CHECK(deadEntityLog[0] == goblin);

		// 验证兽人存活
		CHECK(ApproxEqual(orc.GetComponent<Health>().Current, 110.0f));
		CHECK(!orc.HasComponent<DeadTag>());

		// 验证位置:
		// 哥布林在当帧伤害结算死亡后被打上 DeadTag，因此 MovementSystem (.Without<DeadTag>()) 立即停止其移动
		CHECK(ApproxEqual(goblin.GetComponent<Position2D>().X, 8.0f)); // 保持第 1 帧末的位置，不再位移
		CHECK(ApproxEqual(hero.GetComponent<Position2D>().X, 20.0f));
		CHECK(ApproxEqual(orc.GetComponent<Position2D>().Y, 8.0f));

		// Luau 系统第 2 帧累积: 5.0 + 5.0 = 10.0
		CHECK(ApproxEqual(hero.GetComponent<TransformComponent>().Location.y, 10.0f));

		// === 模拟第 3 帧: 已死实体持续静止，存活实体继续前进 (dt = 1.0s) ===
		scene.OnUpdateRuntime(Timestep(1.0f));

		// 哥布林依然静止
		CHECK(ApproxEqual(goblin.GetComponent<Position2D>().X, 8.0f));
		// 英雄与兽人继续前进
		CHECK(ApproxEqual(hero.GetComponent<Position2D>().X, 30.0f));
		CHECK(ApproxEqual(orc.GetComponent<Position2D>().Y, 7.0f));
		// 死亡回调不重复触发
		CHECK(deadEntityLog.size() == 1);

		// Luau 系统第 3 帧累积: 10.0 + 5.0 = 15.0
		CHECK(ApproxEqual(hero.GetComponent<TransformComponent>().Location.y, 15.0f));

		// -----------------------------------------------------------------
		// 校验 6: Query DSL 现代链式筛选与统计特性专项验证
		// -----------------------------------------------------------------
		// 基础计数与筛选
		CHECK(scene.Query<Position2D>().Count() == 3);
		CHECK(scene.Query<MonsterTag>().Count() == 2);
		CHECK((scene.Query<MonsterTag, DeadTag>().Count() == 1));
		CHECK(scene.Query<MonsterTag>().Without<DeadTag>().Count() == 1); // 仅剩存活的 Orc

		// 链式 Each 遍历所有存活战斗实体
		std::size_t aliveCombatantCount = 0;
		scene.Query<Health, Position2D>()
			.Without<DeadTag>()
			.Each([&aliveCombatantCount, goblin](entt::entity e, const Health& h, const Position2D& p) {
				(void)p;
				CHECK(e != static_cast<entt::entity>(goblin));
				CHECK(h.Current > 0.0f);
				++aliveCombatantCount;
			});
		CHECK(aliveCombatantCount == 2); // Hero 与 Orc

		// 帧系统耗时监控 (GetFrameSystemTimings)
		const auto& timings = scene.GetFrameSystemTimings();
		CHECK(timings.size() >= 7); // 5 大内置系统 + DamageProcessingSystem + MovementSystem + LuauCombatEffectSystem

		std::printf("[World.PureEcsExample] All Pure ECS end-to-end example assertions passed successfully!\n");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[World.PureEcsExample] Test failed with exception: %s\n", e.what());
		return 1;
	}
}
