// Pure ECS M2: 现代 Query DSL 与系统管线统一测试
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Query.h"

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
			CHECK(timings[2].Name == "scene-update");
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

		std::puts("WorldQueryTests passed all assertions!");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "WorldQueryTests failed: %s\n", e.what());
		return 1;
	}
}
