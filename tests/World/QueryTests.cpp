// Pure ECS M2: 现代 Query DSL 与系统管线统一测试
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/ISystem.h"
#include "World/Scene/Query.h"
#include <cmath>

#include <algorithm>
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

	struct VelocityTestComponent
	{
		glm::vec3 Linear{ 0.0f, 0.0f, 0.0f };
	};

	struct DisabledTestTag
	{
	};

	class CustomTestSystem : public World::ISystem
	{
	public:
		std::string_view Name() const override { return "CustomTestSystem"; }
		void Update(World::Scene& scene, World::Timestep dt) override
		{
			++UpdateCount;
			LastDt = dt.GetSeconds();
			auto q = scene.Query<World::TransformComponent>();
			q.Each([](World::TransformComponent& t) {
				t.Location.x += 10.0f;
			});
		}

		int UpdateCount = 0;
		float LastDt = 0.0f;
	};

	// PURE-ECS:阶段与同阶段顺序依赖必须**真的生效**。此前 Scene::FrameSystem 只有
	// {Name, ParallelSafe, Update},注册时被硬编码成 SystemPhase::Update,RunFrameSystems
	// 也只跑 Update ⇒ `ISystem::Phase()` / `After()` 全仓没有任何调用点(等于死 API)。
	class PhaseProbeSystem : public World::ISystem
	{
	public:
		PhaseProbeSystem(std::string name, World::Gameplay::SystemPhase phase,
			std::vector<std::string> after, std::vector<std::string>* log)
			: m_Name(std::move(name)), m_Phase(phase), m_After(std::move(after)), m_Log(log) {}
		std::string_view Name() const override { return m_Name; }
		World::Gameplay::SystemPhase Phase() const override { return m_Phase; }
		std::vector<std::string> After() const override { return m_After; }
		void Update(World::Scene&, World::Timestep) override { m_Log->push_back(m_Name); }
	private:
		std::string m_Name;
		World::Gameplay::SystemPhase m_Phase;
		std::vector<std::string> m_After;
		std::vector<std::string>* m_Log;
	};
}

int main()
{
	try
	{
		using namespace World;

		WorldContext context;
		Scene scene(context);
		auto& registry = scene.GetRegistry();

		// ========================================================
		// 1. 空查询与 Empty()/Count()
		// ========================================================
		{
			auto q = scene.Query<TransformComponent>();
			CHECK(q.Empty());
			CHECK(q.Count() == 0);
		}

		// 创建测试实体
		// E1: Transform
		const entt::entity e1 = registry.create();
		registry.emplace<TransformComponent>(e1, glm::vec3(1.0f, 2.0f, 3.0f));

		// E2: Transform + Tag
		const entt::entity e2 = registry.create();
		registry.emplace<TransformComponent>(e2, glm::vec3(4.0f, 5.0f, 6.0f));
		registry.emplace<TagComponent>(e2, "Entity2");

		// E3: Transform + Tag + VelocityTestComponent
		const entt::entity e3 = registry.create();
		registry.emplace<TransformComponent>(e3, glm::vec3(7.0f, 8.0f, 9.0f));
		registry.emplace<TagComponent>(e3, "Entity3");
		registry.emplace<VelocityTestComponent>(e3, VelocityTestComponent{ glm::vec3(10.0f, 0.0f, 0.0f) });

		// E4: Transform + DisabledTestTag
		const entt::entity e4 = registry.create();
		registry.emplace<TransformComponent>(e4, glm::vec3(10.0f, 20.0f, 30.0f));
		registry.emplace<DisabledTestTag>(e4);

		// ========================================================
		// 2. 基本查询与组件读写 (Func(Components&...))
		// ========================================================
		{
			auto q = scene.Query<TransformComponent>();
			CHECK(!q.Empty());
			CHECK(q.Count() == 4);

			std::size_t visited = 0;
			q.Each([&visited](TransformComponent& transform) {
				transform.Location += glm::vec3(1.0f, 1.0f, 1.0f);
				++visited;
			});
			CHECK(visited == 4);

			// 验证写回成功
			CHECK(registry.get<TransformComponent>(e1).Location == glm::vec3(2.0f, 3.0f, 4.0f));
			CHECK(registry.get<TransformComponent>(e2).Location == glm::vec3(5.0f, 6.0f, 7.0f));
		}

		// ========================================================
		// 3. 包含 entity 签名 (Func(entt::entity, Components&...))
		// ========================================================
		{
			auto q = scene.Query<TransformComponent>();
			std::vector<entt::entity> entities;
			q.Each([&entities](entt::entity entity, TransformComponent& transform) {
				entities.push_back(entity);
				(void)transform;
			});
			CHECK(entities.size() == 4);
			CHECK(entities[0] == e1 || entities[0] == e2 || entities[0] == e3 || entities[0] == e4);
		}

		// ========================================================
		// 4. 多组件查询 (Query<T1, T2...>)
		// ========================================================
		{
			auto q = scene.Query<TransformComponent, TagComponent>();
			CHECK(q.Count() == 2);

			std::size_t visitedCount = 0;
			q.Each([&visitedCount, e2, e3](entt::entity entity, TransformComponent& transform, TagComponent& tag) {
				(void)transform;
				CHECK(entity == e2 || entity == e3);
				CHECK(tag.Tag == "Entity2" || tag.Tag == "Entity3");
				++visitedCount;
			});
			CHECK(visitedCount == 2);
		}

		// ========================================================
		// 5. .Without 排除过滤
		// ========================================================
		{
			// TransformComponent 且排除 DisabledTestTag (应剩余 e1, e2, e3)
			auto q = scene.Query<TransformComponent>().Without<DisabledTestTag>();
			CHECK(q.Count() == 3);

			bool foundDisabled = false;
			q.Each([&foundDisabled, e4](entt::entity entity, TransformComponent& transform) {
				(void)transform;
				if (entity == e4)
					foundDisabled = true;
			});
			CHECK(!foundDisabled);
		}

		// ========================================================
		// 6. .With 包含过滤
		// ========================================================
		{
			// 查询 TransformComponent, 要求持有 VelocityTestComponent (仅 e3)
			// 注意: Each 回调只接受 TransformComponent, 但只匹配持有 VelocityTestComponent 的实体
			auto q = scene.Query<TransformComponent>().With<VelocityTestComponent>();
			CHECK(q.Count() == 1);

			entt::entity visitedEntity = entt::null;
			q.Each([&visitedEntity](entt::entity entity, TransformComponent& transform) {
				visitedEntity = entity;
				CHECK(transform.Location == glm::vec3(8.0f, 9.0f, 10.0f));
			});
			CHECK(visitedEntity == e3);

			// 同时也测试不带 entity 的签名
			std::size_t count = 0;
			q.Each([&count](TransformComponent& transform) {
				(void)transform;
				++count;
			});
			CHECK(count == 1);
		}

		// ========================================================
		// 7. 链式组合 .With 与 .Without
		// ========================================================
		{
			// 持有 TagComponent, 排除 DisabledTestTag
			auto q = scene.Query<TransformComponent>()
				.With<TagComponent>()
				.Without<DisabledTestTag>();
			CHECK(q.Count() == 2);

			// 再加上 Without<VelocityTestComponent> (应仅剩 e2)
			auto q2 = q.Without<VelocityTestComponent>();
			CHECK(q2.Count() == 1);

			entt::entity resultEntity = entt::null;
			q2.Each([&resultEntity](entt::entity entity, TransformComponent&) {
				resultEntity = entity;
			});
			CHECK(resultEntity == e2);
		}

		// ========================================================
		// 8. const 查询版本 (Query<const Components...>)
		// ========================================================
		{
			const Scene& constScene = scene;
			auto constQ = constScene.Query<TransformComponent>();
			CHECK(constQ.Count() == 4);

			std::size_t constVisited = 0;
			constQ.Each([&constVisited](const TransformComponent& transform) {
				(void)transform.Location;
				++constVisited;
			});
			CHECK(constVisited == 4);

			// 带 entity 的 const 签名
			constQ.Each([](entt::entity entity, const TransformComponent& transform) {
				(void)entity;
				(void)transform.Scale;
			});

			// 直接构造 Query<const TransformComponent>
			World::Query<const TransformComponent> directConstQ(registry);
			CHECK(directConstQ.Count() == 4);
		}

		// ========================================================
		// 9. 帧管线系统默认注册验证 (Scene 5 大系统管线调度)
		// ========================================================
		{
			Scene pipeScene(context);
			pipeScene.EnsureDefaultFrameSystems();
			// 触发一次运行时更新
			pipeScene.OnUpdateRuntime(0.016f);

			const auto& timings = pipeScene.GetFrameSystemTimings();
			CHECK(timings.size() == 5);
			CHECK(timings[0].Name == "physics-2d");
			CHECK(timings[1].Name == "physics-3d");
			CHECK(timings[2].Name == "movement-system");
			CHECK(timings[3].Name == "transform-system");
			CHECK(timings[4].Name == "camera-system");
		}

		// ========================================================
		// 10. Entity 流式调用方法 (.Set, .With, .Without 链式操作)
		// ========================================================
		{
			Entity fluentEntity = Entity::CreateEntity(&scene, "FluentEntity");
			CHECK(fluentEntity.IsValid());
			CHECK(fluentEntity.HasComponent<TagComponent>());

			// 链式调用 .Set / .With
			fluentEntity.Set<TransformComponent>(glm::vec3(100.0f, 200.0f, 300.0f))
				.With<VelocityTestComponent>(VelocityTestComponent{ glm::vec3(1.0f, 2.0f, 3.0f) })
				.With<DisabledTestTag>();

			CHECK(fluentEntity.HasComponent<TransformComponent>());
			CHECK(fluentEntity.HasComponent<VelocityTestComponent>());
			CHECK(fluentEntity.HasComponent<DisabledTestTag>());
			CHECK(fluentEntity.GetComponent<TransformComponent>().Location == glm::vec3(100.0f, 200.0f, 300.0f));
			CHECK(fluentEntity.GetComponent<VelocityTestComponent>().Linear == glm::vec3(1.0f, 2.0f, 3.0f));

			// .Set 覆盖已有组件
			fluentEntity.Set<VelocityTestComponent>(VelocityTestComponent{ glm::vec3(4.0f, 5.0f, 6.0f) });
			CHECK(fluentEntity.GetComponent<VelocityTestComponent>().Linear == glm::vec3(4.0f, 5.0f, 6.0f));

			// .Without 链式移除组件
			fluentEntity.Without<DisabledTestTag>();
			CHECK(!fluentEntity.HasComponent<DisabledTestTag>());

			// 链式混合调用
			fluentEntity.Without<VelocityTestComponent>()
				.With<DisabledTestTag>();
			CHECK(!fluentEntity.HasComponent<VelocityTestComponent>());
			CHECK(fluentEntity.HasComponent<DisabledTestTag>());
		}

		// ========================================================
		// 11. ISystem 与 Scene::RegisterSystem 挂载及帧调度验证
		// ========================================================
		{
			Scene systemScene(context);
			auto& sysRef = systemScene.RegisterSystem<CustomTestSystem>();
			CHECK(sysRef.Name() == "CustomTestSystem");
			CHECK(sysRef.UpdateCount == 0);

			const entt::entity testEnt = systemScene.GetRegistry().create();
			systemScene.GetRegistry().emplace<TransformComponent>(testEnt, glm::vec3(1.0f, 2.0f, 3.0f));

			// 触发运行时帧更新: 默认 5 大系统 + 1 个 CustomTestSystem 调度
			systemScene.OnUpdateRuntime(0.016f);

			CHECK(sysRef.UpdateCount == 1);
			CHECK(std::fabs(sysRef.LastDt - 0.016f) < 0.001f);
			CHECK(systemScene.GetRegistry().get<TransformComponent>(testEnt).Location.x == 11.0f);

			const auto& timings = systemScene.GetFrameSystemTimings();
			CHECK(timings.size() == 6);
			CHECK(timings[5].Name == "CustomTestSystem");
		}

		// ========================================================
		// 11b. ISystem 的 Phase / After 真的影响调度(PURE-ECS 修复回归)
		//   修复前:注册被硬编码进 Update 阶段、RunFrameSystems 只跑 Update ⇒
		//   非 Update 阶段的系统**永不执行**,After 也不起作用。
		// ========================================================
		{
			Scene phaseScene(context);
			std::vector<std::string> log;
			// 注册顺序故意与期望执行顺序相反:After 依赖必须把它纠正过来。
			phaseScene.RegisterSystem<PhaseProbeSystem>("SecondSystem",
				Gameplay::SystemPhase::Update, std::vector<std::string> { "FirstSystem" }, &log);
			phaseScene.RegisterSystem<PhaseProbeSystem>("FirstSystem",
				Gameplay::SystemPhase::Update, std::vector<std::string> {}, &log);
			// 非 Update 阶段的系统:修复前 **一次都不会跑**。
			phaseScene.RegisterSystem<PhaseProbeSystem>("LateSystem",
				Gameplay::SystemPhase::Late, std::vector<std::string> {}, &log);
			phaseScene.RegisterSystem<PhaseProbeSystem>("PreFixedSystem",
				Gameplay::SystemPhase::PreFixed, std::vector<std::string> {}, &log);

			phaseScene.OnUpdateRuntime(0.016f);

			CHECK(log.size() == 4);
			// 阶段顺序:PreFixed 先于 Update,Update 先于 Late。
			CHECK(log[0] == "PreFixedSystem");
			// 同阶段内:FirstSystem 在 SecondSystem 之前(After 依赖生效)。
			const auto firstAt = std::find(log.begin(), log.end(), "FirstSystem");
			const auto secondAt = std::find(log.begin(), log.end(), "SecondSystem");
			CHECK(firstAt != log.end() && secondAt != log.end() && firstAt < secondAt);
			CHECK(log[3] == "LateSystem");

			// 每个阶段都进耗时表(读面板按它列系统)。
			const auto& phaseTimings = phaseScene.GetFrameSystemTimings();
			CHECK(phaseTimings.size() == 4 + 5);   // 4 个探针 + 5 个引擎内置
			bool sawLate = false;
			for (const auto& timing : phaseTimings)
				sawLate = sawLate || timing.Name == "LateSystem";
			CHECK(sawLate);
		}

		// ========================================================
		// 12. 响应式组件观察者 (OnAdd / OnRemove / RemoveComponentObserver)
		// ========================================================
		{
			Scene observerScene(context);

			int transformAddedCount = 0;
			Entity lastAddedEntity;
			uint64_t addHandle = observerScene.OnAdd<TransformComponent>([&](Entity e) {
				++transformAddedCount;
				lastAddedEntity = e;
			});
			CHECK(addHandle > 0);

			int tagRemovedCount = 0;
			Entity lastRemovedEntity;
			uint64_t removeHandle = observerScene.OnRemove<TagComponent>([&](Entity e) {
				++tagRemovedCount;
				lastRemovedEntity = e;
			});
			CHECK(removeHandle > 0);

			// 创建实体（带有 TagComponent 与 UUIDComponent）
			Entity testEnt = observerScene.CreateEntityShell("ObserverTestEntity");
			CHECK(testEnt.IsValid());
			CHECK(testEnt.HasComponent<TagComponent>());
			CHECK(transformAddedCount == 0);

			// 动态增加 TransformComponent，断言 OnAdd 接收回调
			testEnt.AddComponent(entt::type_id<TransformComponent>().hash());
			CHECK(transformAddedCount == 1);
			CHECK(lastAddedEntity == testEnt);

			// 移除 TagComponent，断言 OnRemove 接收回调
			testEnt.RemoveComponent<TagComponent>();
			CHECK(tagRemovedCount == 1);
			CHECK(lastRemovedEntity == testEnt);

			// 注销观察者后再次操作，断言不再触发
			observerScene.RemoveComponentObserver(addHandle);
			observerScene.RemoveComponentObserver(removeHandle);

			Entity testEnt2 = observerScene.CreateEntityShell("ObserverTestEntity2");
			testEnt2.AddComponent(entt::type_id<TransformComponent>().hash());
			CHECK(transformAddedCount == 1);

			testEnt2.RemoveComponent<TagComponent>();
			CHECK(tagRemovedCount == 1);
		}

		// ========================================================
		// 13. 真实 VelocityComponent 与 MovementSystem 自动位移驱动验证
		// ========================================================
		{
			Scene moveScene(context);
			Entity mover = Entity::CreateEntity(&moveScene, "Mover")
				.Set<TransformComponent>(glm::vec3(0.0f, 0.0f, 0.0f))
				.With<VelocityComponent>(glm::vec3(10.0f, 20.0f, 30.0f));

			moveScene.OnUpdateRuntime(0.1f);
			CHECK(mover.GetComponent<TransformComponent>().Location.x == 1.0f);
			CHECK(mover.GetComponent<TransformComponent>().Location.y == 2.0f);
			CHECK(mover.GetComponent<TransformComponent>().Location.z == 3.0f);
		}

		std::puts("WorldQueryTests passed all assertions!");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "WorldQueryTests failed: %s\n", e.what());
		return 1;
	}
}
