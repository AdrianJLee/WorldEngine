// Pure ECS M3: Luau 纯 ECS 绑定测试 (ecs / world 全局表、Query DSL、AddSystem、EntityCount)
#include "wldpch.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Scene/ScriptEngine.h"
#include "World/Script/LuauVm.h"
#include "World/Script/ScriptBindingContext.h"
#include "World/Script/ScriptRef.h"
#include "World/Script/ScriptValue.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
	using namespace World;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	void RunOk(const std::string& source, const char* chunk, int line)
	{
		std::string error;
		if (!ScriptEngine::GetState().RunString(source, chunk, &error))
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + chunk +
				" failed: " + error);
	}
#define RUN_OK(source, chunk) RunOk(source, chunk, __LINE__)

	bool ApproxEqual(float a, float b, float eps = 0.001f)
	{
		return std::fabs(a - b) <= eps;
	}
}

int main()
{
	try
	{
		ScriptEngine::Init();
		WorldContext context;

		// =====================================================================
		// 1. 全局 ecs 与 world 表验证 (存在性、同一表引用、只读性)
		// =====================================================================
		{
			RUN_OK(R"(
				assert(type(ecs) == "table", "ecs must be a table")
				assert(type(world) == "table", "world must be a table")
				assert(ecs == world, "ecs and world must be the same table reference")
				assert(type(ecs.Query) == "function", "ecs.Query must be a function")
				assert(type(ecs.AddSystem) == "function", "ecs.AddSystem must be a function")
				assert(type(ecs.EntityCount) == "function", "ecs.EntityCount must be a function")
				assert(type(ecs.CreateEntity) == "function", "ecs.CreateEntity must be a function")
				assert(type(ecs.DestroyEntity) == "function", "ecs.DestroyEntity must be a function")
				assert(type(world.Query) == "function", "world.Query must be a function")
				assert(type(world.AddSystem) == "function", "world.AddSystem must be a function")
				assert(type(world.EntityCount) == "function", "world.EntityCount must be a function")
				assert(type(world.CreateEntity) == "function", "world.CreateEntity must be a function")
				assert(type(world.DestroyEntity) == "function", "world.DestroyEntity must be a function")

				-- Read-only verification
				local okWriteEcs = pcall(function() ecs.NewField = 123 end)
				assert(not okWriteEcs, "writing to ecs must fail")
				local okWriteWorld = pcall(function() world.NewField = 456 end)
				assert(not okWriteWorld, "writing to world must fail")
			)", "TestEcsGlobals");
		}

		// =====================================================================
		// 2. ecs:EntityCount() 与 world:EntityCount() 实体总数查询
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			RUN_OK(R"(
				assert(ecs:EntityCount() == 0, "initial entity count must be 0")
				assert(world:EntityCount() == 0, "initial entity count via world must be 0")
				assert(ecs.EntityCount() == 0, "dot syntax EntityCount must work")
			)", "TestInitialEntityCount");

			Entity e1 = scene.CreateEntityShell("E1");
			Entity e2 = scene.CreateEntityShell("E2");
			Entity e3 = scene.CreateEntityShell("E3");
			CHECK(e1.IsValid() && e2.IsValid() && e3.IsValid());

			RUN_OK(R"(
				assert(ecs:EntityCount() == 3, "entity count after creating 3 must be 3")
				assert(world:EntityCount() == 3, "entity count via world must be 3")
				assert(ecs.EntityCount() == 3, "dot syntax entity count must be 3")
			)", "TestCreatedEntityCount");
		}

		// =====================================================================
		// 3. ecs:Query 单组件查询、遍历与属性写回
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity e1 = scene.CreateEntityShell("Entity1");
			auto& t1 = e1.AddComponent<TransformComponent>();
			t1.Location = glm::vec3(1.0f, 2.0f, 3.0f);

			Entity e2 = scene.CreateEntityShell("Entity2");
			auto& t2 = e2.AddComponent<TransformComponent>();
			t2.Location = glm::vec3(4.0f, 5.0f, 6.0f);

			Entity e3 = scene.CreateEntityShell("EntityNoTransform");
			// e3 不挂载 TransformComponent

			RUN_OK(R"(
				local q = ecs:Query({"TransformComponent"})
				assert(q ~= nil, "query must return a query object")
				assert(q:Count() == 2, "transform component count must be 2")
				assert(q.Count() == 2, "dot syntax Count must be 2")

				local visited = 0
				q:Each(function(entity, transform)
					visited = visited + 1
					assert(entity:IsValid(), "entity must be valid")
					assert(transform ~= nil, "transform must not be nil")
					transform.Location = transform.Location + vec3.new(10.0, 10.0, 10.0)
				end)
				assert(visited == 2, "query:Each must visit exactly 2 entities")
			)", "TestQueryEachModify");

			// 验证 C++ 侧场景中的 TransformComponent 确实已被修改
			const auto& resT1 = e1.GetComponent<TransformComponent>();
			CHECK(ApproxEqual(resT1.Location.x, 11.0f));
			CHECK(ApproxEqual(resT1.Location.y, 12.0f));
			CHECK(ApproxEqual(resT1.Location.z, 13.0f));

			const auto& resT2 = e2.GetComponent<TransformComponent>();
			CHECK(ApproxEqual(resT2.Location.x, 14.0f));
			CHECK(ApproxEqual(resT2.Location.y, 15.0f));
			CHECK(ApproxEqual(resT2.Location.z, 16.0f));
		}

		// =====================================================================
		// 4. ecs:Query 多组件联合过滤查询
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity e1 = scene.CreateEntityShell("EntityA");
			auto& t1 = e1.AddComponent<TransformComponent>();
			t1.Location = glm::vec3(10.0f, 20.0f, 30.0f);
			// CreateEntityShell 已带 TagComponent，Tag="EntityA"

			Entity e2 = scene.CreateEntityShell("EntityB");
			// e2 有 TagComponent，但不挂 TransformComponent

			Entity e3 = scene.CreateEntityShell("EntityC");
			auto& t3 = e3.AddComponent<TransformComponent>();
			t3.Location = glm::vec3(40.0f, 50.0f, 60.0f);
			// e3 有 TagComponent 和 TransformComponent

			RUN_OK(R"(
				local q = ecs:Query({"TransformComponent", "TagComponent"})
				assert(q:Count() == 2, "entities with both Transform and Tag must be 2")

				local count = 0
				q:Each(function(entity, transform, tag)
					count = count + 1
					assert(entity:IsValid())
					assert(transform ~= nil)
					assert(tag ~= nil)
					assert(tag.Tag == "EntityA" or tag.Tag == "EntityC")
				end)
				assert(count == 2)
			)", "TestMultiComponentQuery");
		}

		// =====================================================================
		// 5. ecs:AddSystem 注册系统并在 Scene::OnUpdateRuntime 时被回调
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity mover = scene.CreateEntityShell("Mover");
			auto& transform = mover.AddComponent<TransformComponent>();
			transform.Location = glm::vec3(0.0f, 0.0f, 0.0f);

			RUN_OK(R"(
				local ok = ecs:AddSystem("LuauMovementSystem", "Update", function(dt)
					local q = ecs:Query({"TransformComponent"})
					q:Each(function(entity, t)
						t.Location = t.Location + vec3.new(10.0 * dt, 0.0, 0.0)
					end)
				end)
				assert(ok == true, "AddSystem must return true")
			)", "TestAddSystemRegister");

			// 模拟第一帧更新: dt = 0.1s -> x += 1.0
			scene.OnUpdateRuntime(Timestep(0.1f));
			CHECK(ApproxEqual(mover.GetComponent<TransformComponent>().Location.x, 1.0f));

			// 模拟第二帧更新: dt = 0.2s -> x += 2.0 (累积 3.0)
			scene.OnUpdateRuntime(Timestep(0.2f));
			CHECK(ApproxEqual(mover.GetComponent<TransformComponent>().Location.x, 3.0f));
		}

		// =====================================================================
		// 6. world 别名同等性验证 (通过 world:Query 与 world:AddSystem 执行)
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity box = scene.CreateEntityShell("Box");
			auto& transform = box.AddComponent<TransformComponent>();
			transform.Location = glm::vec3(5.0f, 5.0f, 5.0f);

			RUN_OK(R"(
				local q = world:Query({"TransformComponent"})
				assert(q:Count() == 1)

				world:AddSystem("WorldAliasSystem", function(dt)
					q:Each(function(e, t)
						t.Location = t.Location + vec3.new(0.0, 5.0 * dt, 0.0)
					end)
				end)
			)", "TestWorldAliasSystem");

			scene.OnUpdateRuntime(Timestep(0.2f));
			CHECK(ApproxEqual(box.GetComponent<TransformComponent>().Location.y, 6.0f));
		}

		// =====================================================================
		// 7. 异常与错误处理 (非法组件名、无效参数、未设活动场景等)
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			RUN_OK(R"(
				-- 未知组件名称报错
				local okUnknown = pcall(function()
					ecs:Query({"NonExistentComponentType12345"})
				end)
				assert(not okUnknown, "Querying unknown component must fail")

				-- 空数组报错
				local okEmpty = pcall(function()
					ecs:Query({})
				end)
				assert(not okEmpty, "Querying empty array must fail")

				-- 非法 AddSystem 参数报错
				local okBadSys = pcall(function()
					ecs:AddSystem("BadSystem", "not_a_function")
				end)
				assert(not okBadSys, "AddSystem with invalid callback must fail")
			)", "TestErrorHandling");

			// 清除活动场景后调用 Query 必须抛出异常
			ScriptEngine::SetActiveScene(nullptr);
			RUN_OK(R"(
				local okNoScene = pcall(function()
					ecs:Query({"TransformComponent"})
				end)
				assert(not okNoScene, "Query without active scene must fail")
			)", "TestNoActiveScene");
		}

		// =====================================================================
		// 8. ecs:CreateEntity 与 ecs:DestroyEntity 实体生命周期绑定测试
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			RUN_OK(R"(
				assert(ecs:EntityCount() == 0)

				-- 1. 默认名字创建实体
				local e1 = ecs:CreateEntity()
				assert(e1 ~= nil, "CreateEntity must return userdata")
				assert(e1:IsValid(), "e1 must be valid")
				assert(e1:GetName() == "Empty Entity", "default entity name must be 'Empty Entity'")
				assert(ecs:EntityCount() == 1, "entity count must be 1")

				-- 2. 指定名称创建实体
				local e2 = ecs:CreateEntity("CustomPlayer")
				assert(e2 ~= nil and e2:IsValid())
				assert(e2:GetName() == "CustomPlayer", "entity name must match")
				assert(ecs:EntityCount() == 2)

				-- 3. world 别名创建实体
				local e3 = world:CreateEntity("WorldEntity")
				assert(e3 ~= nil and e3:IsValid())
				assert(e3:GetName() == "WorldEntity")
				assert(world:EntityCount() == 3)

				-- 4. 销毁实体 e1
				ecs:DestroyEntity(e1)
				assert(ecs:EntityCount() == 2, "entity count after destroying e1 must be 2")

				-- 5. 通过 world 别名销毁实体 e2
				world:DestroyEntity(e2)
				assert(ecs:EntityCount() == 1, "entity count after destroying e2 must be 1")

				-- 6. 点号语法调用销毁实体 e3
				ecs.DestroyEntity(e3)
				assert(ecs:EntityCount() == 0, "entity count after destroying e3 must be 0")
			)", "TestCreateAndDestroyEntity");

			// 验证异常与非法参数
			RUN_OK(R"(
				-- 无参数调用 DestroyEntity
				local okNoArg = pcall(function() ecs:DestroyEntity() end)
				assert(not okNoArg, "DestroyEntity without arguments must fail")

				-- 非 Entity 参数调用 DestroyEntity
				local okBadType = pcall(function() ecs:DestroyEntity("not_an_entity") end)
				assert(not okBadType, "DestroyEntity with invalid type must fail")
			)", "TestCreateDestroyErrors");
		}

		ScriptEngine::Shutdown();
		std::puts("World.LuauEcs: all tests passed!");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "World.LuauEcs failed: %s\n", e.what());
		return 1;
	}
}
