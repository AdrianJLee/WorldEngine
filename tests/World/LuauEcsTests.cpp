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
#include "World/Utils/Paths.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
				assert(type(ecs.OnAdd) == "function", "ecs.OnAdd must be a function")
				assert(type(ecs.OnRemove) == "function", "ecs.OnRemove must be a function")
				assert(type(ecs.Off) == "function", "ecs.Off must be a function")
				assert(type(world.Query) == "function", "world.Query must be a function")
				assert(type(world.AddSystem) == "function", "world.AddSystem must be a function")
				assert(type(world.EntityCount) == "function", "world.EntityCount must be a function")
				assert(type(world.CreateEntity) == "function", "world.CreateEntity must be a function")
				assert(type(world.DestroyEntity) == "function", "world.DestroyEntity must be a function")
				assert(type(world.OnAdd) == "function", "world.OnAdd must be a function")
				assert(type(world.OnRemove) == "function", "world.OnRemove must be a function")
				assert(type(world.Off) == "function", "world.Off must be a function")

				-- Read-only verification
				local okWriteEcs = pcall(function() ecs.NewField = 123 end)
				assert(not okWriteEcs, "writing to ecs must fail")
				local okWriteWorld = pcall(function() world.NewField = 456 end)
				assert(not okWriteWorld, "writing to world must fail")

				-- Comp / Components 常量表验证
				assert(type(Comp) == "table", "Comp must be a table")
				assert(type(Components) == "table", "Components must be a table")
				assert(Comp == Components, "Comp and Components must be the same table")
				assert(Comp.Transform == "TransformComponent", "Comp.Transform must map to TransformComponent")
				assert(Comp.TransformComponent == "TransformComponent", "Comp.TransformComponent must map to TransformComponent")
				assert(Comp.Velocity == "VelocityComponent", "Comp.Velocity must map to VelocityComponent")
				assert(Comp.Camera == "CameraComponent", "Comp.Camera must map to CameraComponent")
				local okWriteComp = pcall(function() Comp.NewComp = "test" end)
				assert(not okWriteComp, "writing to Comp must fail")

				-- Phase 阶段枚举验证
				assert(type(Phase) == "table", "Phase must be a table")
				assert(Phase.PreFixed == "PreFixed", "Phase.PreFixed must be PreFixed")
				assert(Phase.Fixed == "Fixed", "Phase.Fixed must be Fixed")
				assert(Phase.Update == "Update", "Phase.Update must be Update")
				assert(Phase.Late == "Late", "Phase.Late must be Late")
				assert(Phase.PreRender == "PreRender", "Phase.PreRender must be PreRender")
				local okWritePhase = pcall(function() Phase.NewPhase = "test" end)
				assert(not okWritePhase, "writing to Phase must fail")
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
		// 4b. ecs:Query 排除过滤 (第二参数选项表 without)
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity alive = scene.CreateEntityShell("Alive");
			alive.AddComponent<TransformComponent>();

			Entity dead = scene.CreateEntityShell("Dead");
			dead.AddComponent<TransformComponent>();
			dead.AddComponent<VelocityComponent>();

			RUN_OK(R"(
				-- 旧行为不变:单参数查询仍然命中全部
				local all = ecs:Query({"TransformComponent"})
				assert(all:Count() == 2, "single-argument query must still match all 2 entities")

				-- without 排除带 VelocityComponent 的实体
				local aliveOnly = ecs:Query({"TransformComponent"}, { without = {"VelocityComponent"} })
				assert(aliveOnly:Count() == 1, "without must exclude the entity carrying VelocityComponent")

				local visited = 0
				aliveOnly:Each(function(entity, transform)
					visited = visited + 1
					assert(entity:GetName() == "Alive", "only the Alive entity must remain")
					assert(transform ~= nil, "transform proxy must not be nil")
				end)
				assert(visited == 1, "Each must visit exactly one entity after exclusion")

				-- world 别名与字符串组件名同样支持
				local aliasCount = world:Query("TransformComponent", { without = {"VelocityComponent"} }):Count()
				assert(aliasCount == 1, "world alias with string component and without must exclude")

				-- 冒号 + 连续的字符串组件名(旧形态)
				local multiString = ecs:Query("TransformComponent", "VelocityComponent"):Count()
				assert(multiString == 1, "colon call with multiple string component names must work")

				-- 点调用(无 self):数组形态与"数组 + 选项表"都必须正常
				local dotCount = ecs.Query({"TransformComponent"}):Count()
				assert(dotCount == 2, "dot call without self must match both entities")

				local dotOptions = ecs.Query({"TransformComponent"}, { without = {"VelocityComponent"} }):Count()
				assert(dotOptions == 1, "dot call with options table must exclude")

				local dotAlias = world.Query({"TransformComponent"}, { without = {"VelocityComponent"} }):Count()
				assert(dotAlias == 1, "world dot call with options table must exclude")
			)", "TestQueryWithout");

			RUN_OK(R"(
				-- without 里的未注册组件名必须报可读错误(消息里带该名字)
				local okUnknown, unknownErr = pcall(function()
					ecs:Query({"TransformComponent"}, { without = {"NoSuchComponentXYZ"} })
				end)
				assert(not okUnknown, "without with an unregistered component must fail")
				assert(string.find(unknownErr, "NoSuchComponentXYZ", 1, true) ~= nil,
					"error must name the unknown component: " .. tostring(unknownErr))

				-- 选项表未知键必须报可读错误(消息里列出合法键)
				local okUnknownKey, keyErr = pcall(function()
					ecs:Query({"TransformComponent"}, { wtihout = {"VelocityComponent"} })
				end)
				assert(not okUnknownKey, "unknown option key must fail")
				assert(string.find(keyErr, "unknown option key", 1, true) ~= nil,
					"error must mention the unknown option key: " .. tostring(keyErr))

				-- without 的值必须是数组
				local okBadWithout = pcall(function()
					ecs:Query({"TransformComponent"}, { without = 5 })
				end)
				assert(not okBadWithout, "without must be an array of names")

				-- 选项位置不能是数字(字符串/表都算组件名,故用数字触发)
				local okBadOptions = pcall(function()
					ecs:Query({"TransformComponent"}, 5)
				end)
				assert(not okBadOptions, "options argument must be a table")
			)", "TestQueryWithoutErrors");
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

		// =====================================================================
		// 6. 响应式组件观察者 ecs:OnAdd / ecs:OnRemove / ecs:Off
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			RUN_OK(R"(
				local addTriggerCount = 0
				local lastAddedName = ""
				local handleAdd = ecs:OnAdd("TagComponent", function(ent)
					addTriggerCount = addTriggerCount + 1
					assert(ent:HasComponent("TagComponent"), "added entity must have TagComponent")
					lastAddedName = ent:GetName()
				end)
				assert(type(handleAdd) == "number" and handleAdd > 0, "handleAdd must be a positive number")

				local removeTriggerCount = 0
				local handleRemove = world:OnRemove("TagComponent", function(ent)
					removeTriggerCount = removeTriggerCount + 1
				end)
				assert(type(handleRemove) == "number" and handleRemove > 0, "handleRemove must be a positive number")

				-- 1. 创建实体时触发 TagComponent OnAdd
				local e1 = ecs:CreateEntity("ObservedE1")
				assert(addTriggerCount == 1, "OnAdd should trigger on entity creation")
				assert(lastAddedName == "ObservedE1", "entity name in callback should match")

				-- 2. 点号语法创建实体也应触发
				local e2 = ecs.CreateEntity("ObservedE2")
				assert(addTriggerCount == 2, "OnAdd should trigger for e2")
				assert(lastAddedName == "ObservedE2", "entity name for e2 should match")

				-- 3. 移除组件触发 OnRemove
				assert(removeTriggerCount == 0, "removeTriggerCount must initially be 0")
				e1:RemoveComponent("TagComponent")
				assert(removeTriggerCount == 1, "OnRemove should trigger on component removal")

				-- 4. 通过 ecs:Off 注销观察者
				local offAddOk = ecs:Off(handleAdd)
				assert(offAddOk == true, "ecs:Off should return true")
				local offRemoveOk = world:Off(handleRemove)
				assert(offRemoveOk == true, "world:Off should return true")

				-- 5. 注销后创建实体和移除组件不再触发
				local e3 = ecs:CreateEntity("ObservedE3")
				assert(addTriggerCount == 2, "OnAdd should not trigger after Off")

				e2:RemoveComponent("TagComponent")
				assert(removeTriggerCount == 1, "OnRemove should not trigger after Off")

				-- 6. 非法参数校验
				local okBadComp = pcall(function() ecs:OnAdd("NoSuchComponent", function() end) end)
				assert(not okBadComp, "OnAdd with unregistered component must fail")

				local okNoFn = pcall(function() ecs:OnAdd("TagComponent") end)
				assert(not okNoFn, "OnAdd without callback function must fail")

				local okBadOff = ecs:Off(0)
				assert(okBadOff == false, "Off with invalid handle 0 should return false")
			)", "TestEcsObservers");
		}

		// =====================================================================
		// 7. 系统注册的幂等性 / ecs:RemoveSystem / 系统脚本加载器幂等
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			const auto countSystem = [&scene](const std::string& name)
			{
				for (const auto& timing : scene.GetFrameSystemTimings())
					if (timing.Name == name)
						return true;
				return false;
			};

			// 7.1 ecs:AddSystem 幂等:同名重复注册不抛错(同一场景二次启动/热重载都要能跑)。
			RUN_OK(R"(
				local ticks = 0
				local function noop(dt) ticks = ticks + 1 end
				ecs:AddSystem("IdempotentSystem", "Update", noop)
				ecs:AddSystem("IdempotentSystem", "Update", noop)
				assert(ecs:EntityCount() >= 0, "scene query still works after re-register")
			)", "TestAddSystemIdempotent");
			scene.OnUpdateRuntime(0.016f);
			CHECK(countSystem("IdempotentSystem"));

			// 7.2 ecs:RemoveSystem:撤销后可再注册;撤销不存在的名字返回 false。
			RUN_OK(R"(
				assert(ecs:RemoveSystem("IdempotentSystem") == true, "RemoveSystem of a live system returns true")
				assert(ecs:RemoveSystem("IdempotentSystem") == false, "RemoveSystem of a missing system returns false")
				local okBad = pcall(function() ecs:RemoveSystem("") end)
				assert(not okBad, "RemoveSystem with an empty name must fail")
			)", "TestRemoveSystem");

			// 7.3 加载器幂等:同一目录跑两遍,系统集合不重复、不抛错。
			ScriptEngine::UnloadSystemScripts(scene);
			ScriptEngine::LoadSystemScripts(scene);
			scene.OnUpdateRuntime(0.016f);
			const std::size_t firstPass = scene.GetFrameSystemTimings().size();
			ScriptEngine::LoadSystemScripts(scene);
			scene.OnUpdateRuntime(0.016f);
			CHECK(scene.GetFrameSystemTimings().size() == firstPass);
		}

		// =====================================================================
		// 9. ecs:AddSystem 三种调用形态兼容矩阵(旧行为不回归 + 表形态新能力)
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity probe = scene.CreateEntityShell("CallFormProbe");
			probe.AddComponent<VelocityComponent>();

			const auto countTiming = [&scene](const std::string& wanted)
			{
				std::size_t found = 0;
				for (const auto& timing : scene.GetFrameSystemTimings())
					if (timing.Name == wanted)
						++found;
				return found;
			};

			RUN_OK(R"(
				-- 形态 1:(name, fn) —— phase 默认 Update
				ecs:AddSystem("CompatTwoArg", function(dt)
					local q = ecs:Query({"VelocityComponent"})
					q:Each(function(entity, v) v.Linear = vec3.new(v.Linear.x + 1.0, 0.0, 0.0) end)
				end)

				-- 形态 2:(name, fn, "Update") —— 字符串 phase
				ecs:AddSystem("CompatStringPhase", function(dt)
					local q = ecs:Query({"VelocityComponent"})
					q:Each(function(entity, v) v.Linear = vec3.new(v.Linear.x + 2.0, 0.0, 0.0) end)
				end, "Update")

				-- 形态 3:(name, "Update", fn) —— 最旧参数序
				ecs:AddSystem("CompatLegacyOrder", "Update", function(dt)
					local q = ecs:Query({"VelocityComponent"})
					q:Each(function(entity, v) v.Linear = vec3.new(v.Linear.x + 4.0, 0.0, 0.0) end)
				end)

				-- 幂等替换对表形态同样成立:同名再注册一次不产生第二份系统
				ecs:AddSystem("CompatTableIdempotent", function(dt) end, { phase = "Late" })
				ecs:AddSystem("CompatTableIdempotent", function(dt) end, { phase = "Late" })
			)", "TestAddSystemCallForms");

			scene.OnUpdateRuntime(Timestep(0.016f));

			// 三种形态都真的注册并执行了(三个写入累积:1 + 2 + 4)
			CHECK(ApproxEqual(probe.GetComponent<VelocityComponent>().Linear.x, 7.0f));
			CHECK(countTiming("CompatTwoArg") == 1u);
			CHECK(countTiming("CompatStringPhase") == 1u);
			CHECK(countTiming("CompatLegacyOrder") == 1u);
			CHECK(countTiming("CompatTableIdempotent") == 1u);
		}

		// =====================================================================
		// 10. ecs:AddSystem 表形态:phase = "Late" 真的在 Late 阶段执行
		//     (此前 RunFrameSystems 只跑 Update ⇒ 非 Update 阶段的系统永不执行)
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity probe = scene.CreateEntityShell("PhaseProbe");
			probe.AddComponent<VelocityComponent>();

			const auto hasTiming = [&scene](const std::string& wanted)
			{
				for (const auto& timing : scene.GetFrameSystemTimings())
					if (timing.Name == wanted)
						return true;
				return false;
			};

			RUN_OK(R"(
				ecs:AddSystem("UpdateWriter", function(dt)
					local q = ecs:Query({"VelocityComponent"})
					q:Each(function(entity, v) v.Linear = vec3.new(1.0, 0.0, 0.0) end)
				end, { phase = "Update" })

				ecs:AddSystem("LateWriter", function(dt)
					local q = ecs:Query({"VelocityComponent"})
					q:Each(function(entity, v) v.Linear = vec3.new(2.0, 0.0, 0.0) end)
				end, { phase = "Late" })
			)", "TestAddSystemPhaseTable");

			scene.OnUpdateRuntime(Timestep(0.016f));

			// 最终值来自 LateWriter:Late 阶段确实执行了,且排在 Update 之后
			// (若 Late 不执行 → 1.0;若 Late 抢在 Update 之前 → 1.0)。
			CHECK(ApproxEqual(probe.GetComponent<VelocityComponent>().Linear.x, 2.0f));
			CHECK(hasTiming("UpdateWriter"));
			CHECK(hasTiming("LateWriter"));
		}

		// =====================================================================
		// 11. ecs:AddSystem after 真的纠正同阶段执行顺序(注册顺序与期望顺序相反)
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity probe = scene.CreateEntityShell("OrderProbe");
			probe.AddComponent<VelocityComponent>();

			RUN_OK(R"(
				-- 先注册声明 after OrderB 的 OrderA:After 生效 ⇒ OrderB 先跑,最终值是 OrderA 的 1.0;
				-- 若 After 被忽略 ⇒ OrderA 先跑、OrderB 后写 2.0。
				ecs:AddSystem("OrderA", function(dt)
					local q = ecs:Query({"VelocityComponent"})
					q:Each(function(entity, v) v.Linear = vec3.new(1.0, 0.0, 0.0) end)
				end, { phase = "Update", after = { "OrderB" } })

				ecs:AddSystem("OrderB", function(dt)
					local q = ecs:Query({"VelocityComponent"})
					q:Each(function(entity, v) v.Linear = vec3.new(2.0, 0.0, 0.0) end)
				end, { phase = "Update" })
			)", "TestAddSystemAfterOrder");

			scene.OnUpdateRuntime(Timestep(0.016f));
			CHECK(ApproxEqual(probe.GetComponent<VelocityComponent>().Linear.x, 1.0f));
		}

		// =====================================================================
		// 11b. Comp 常量表与 AddSystem 单表配置形态验证
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity mover = scene.CreateEntityShell("CompMover");
			auto& transform = mover.AddComponent<TransformComponent>();
			transform.Location = glm::vec3(0.0f, 0.0f, 0.0f);

			RUN_OK(R"(
				local ok1 = ecs:AddSystem({
					name = "CompMoverSystem",
					phase = Phase.Update,
					update = function(dt)
						local q = ecs:Query({ Comp.Transform })
						q:Each(function(entity, t)
							t.Location = t.Location + vec3.new(5.0 * dt, 0.0, 0.0)
						end)
					end
				})
				assert(ok1 == true, "AddSystem with table must return true")
			)", "TestCompAndTableAddSystem");

			scene.OnUpdateRuntime(Timestep(0.2f));
			CHECK(ApproxEqual(mover.GetComponent<TransformComponent>().Location.x, 1.0f));
		}

		// =====================================================================
		// 12. ecs:AddSystem 表形态的错误处理(非法 phase / 未知键 / after 类型)
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			RUN_OK(R"(
				local noop = function(dt) end

				-- 非法 phase(表形态):消息里要能看出合法取值
				local okPhase, phaseErr = pcall(function()
					ecs:AddSystem("BadPhase", noop, { phase = "Nope" })
				end)
				assert(not okPhase, "illegal phase must fail")
				assert(string.find(phaseErr, "Nope", 1, true) ~= nil, "error must name the bad phase")
				assert(string.find(phaseErr, "PreRender", 1, true) ~= nil, "error must list the legal phases")

				-- 非法 phase(字符串形态)
				local okPhaseString = pcall(function() ecs:AddSystem("BadPhaseString", noop, "Nope") end)
				assert(not okPhaseString, "illegal phase string must fail")

				-- 非法 phase(最旧参数序)
				local okPhaseLegacy = pcall(function() ecs:AddSystem("BadPhaseLegacy", "Nope", noop) end)
				assert(not okPhaseLegacy, "illegal legacy phase must fail")

				-- 选项表未知键
				local okKey, keyErr = pcall(function()
					ecs:AddSystem("BadKey", noop, { phaze = "Late" })
				end)
				assert(not okKey, "unknown option key must fail")
				assert(string.find(keyErr, "unknown option key", 1, true) ~= nil, "error must mention the unknown key")

				-- after 元素不是字符串
				local okAfterType = pcall(function()
					ecs:AddSystem("BadAfterType", noop, { phase = "Update", after = { 123 } })
				end)
				assert(not okAfterType, "after elements must be strings")

				-- after 不是数组
				local okAfterShape = pcall(function()
					ecs:AddSystem("BadAfterShape", noop, { after = 5 })
				end)
				assert(not okAfterShape, "after must be an array")

				-- phase 不是字符串
				local okPhaseType = pcall(function()
					ecs:AddSystem("BadPhaseType", noop, { phase = 5 })
				end)
				assert(not okPhaseType, "phase must be a string")

				-- 第三参数既不是字符串也不是表
				local okThirdType = pcall(function() ecs:AddSystem("BadThird", noop, 5) end)
				assert(not okThirdType, "third argument must be a phase string or an options table")
			)", "TestAddSystemOptionsErrors");

			// 报错的注册一个都不许落地
			scene.OnUpdateRuntime(Timestep(0.016f));
			for (const auto& timing : scene.GetFrameSystemTimings())
			{
				CHECK(timing.Name != "BadPhase");
				CHECK(timing.Name != "BadPhaseString");
				CHECK(timing.Name != "BadPhaseLegacy");
				CHECK(timing.Name != "BadKey");
				CHECK(timing.Name != "BadAfterType");
				CHECK(timing.Name != "BadAfterShape");
				CHECK(timing.Name != "BadPhaseType");
				CHECK(timing.Name != "BadThird");
			}
		}

		// =====================================================================
		// 13. T13:scripts/lib 受限加载通道 ecs:RequireLib
		//     (正例/缓存/跨 chunk 缓存/内容热重载/逃逸与非法名/循环依赖/沙箱不变/失败不入缓存)
		// =====================================================================
		{
			const std::filesystem::path contentRoot = std::filesystem::temp_directory_path() /
				("worldengine-luau-ecs-lib-" + std::to_string(static_cast<unsigned long long>(GetCurrentProcessId())));
			std::error_code removeError;
			std::filesystem::remove_all(contentRoot, removeError);

			const auto writeLib = [](const std::filesystem::path& path, const std::string& text)
			{
				std::error_code ignored;
				std::filesystem::create_directories(path.parent_path(), ignored);
				std::ofstream file(path, std::ios::binary | std::ios::out | std::ios::trunc);
				file << text;
			};

			const std::filesystem::path libRoot = contentRoot / "scripts" / "lib";
			writeLib(libRoot / "util" / "math.luau",
				"local M = { kind = 'luau' }\n"
				"M.stamp = tostring({})\n"
				"function M.add(a, b) return a + b end\n"
				"return M\n");
			// .luau 优先的对照:同名的 .lua 必须被忽略
			writeLib(libRoot / "util" / "math.lua", "return { kind = 'lua-shadow' }\n");
			writeLib(libRoot / "fallback.lua", "return { kind = 'lua' }\n");
			writeLib(libRoot / "sandbox_probe.luau",
				"return { io = io, os = os, require = require, load = load, dofile = dofile,\n"
				"         loadstring = loadstring, loadfile = loadfile, package = package, debug = debug,\n"
				"         ecsTable = ecs }\n");
			writeLib(libRoot / "broken.luau", "return { this is not lua\n");
			writeLib(libRoot / "reloadme.luau", "return { n = 1 }\n");
			writeLib(libRoot / "cycle_a.luau", "local b = ecs:RequireLib('cycle_b')\nreturn { b = b }\n");
			writeLib(libRoot / "cycle_b.luau", "local a = ecs:RequireLib('cycle_a')\nreturn { a = a }\n");
			// 目录而非文件:候选 scripts/lib/adirlib.luau 的位置放一个目录
			std::error_code directoryError;
			std::filesystem::create_directories(libRoot / "adirlib.luau", directoryError);

			World::Paths::SetAssetRootOverride(contentRoot);
			struct ContentRootScope
			{
				std::filesystem::path Root;
				~ContentRootScope()
				{
					World::Paths::SetAssetRootOverride(std::filesystem::path());
					std::error_code ignored;
					std::filesystem::remove_all(Root, ignored);
				}
			} contentRootScope{ contentRoot };

			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			RUN_OK(R"(
				-- 正例:.luau 优先(同名 .lua 被忽略)、返回值可用
				local math = ecs:RequireLib("util/math")
				assert(type(math) == "table", "RequireLib must return the module table")
				assert(math.kind == "luau", "the .luau library must win over the same-named .lua")
				assert(math.add(2, 3) == 5, "module functions must work")
				assert(type(math.stamp) == "string", "module must stamp its execution")

				-- 模块语义:第二次调用返回同一个值(同一份 stamp ⇒ 模块体只执行一次)
				local again = ecs:RequireLib("util/math")
				assert(again == math, "second call must return the same table")
				assert(again.stamp == math.stamp, "module body must execute only once")

				-- 点调用(无 self)与 world 别名共享同一份缓存
				assert(ecs.RequireLib("util/math") == math, "dot call must hit the same cache entry")
				assert(world:RequireLib("util/math") == math, "world alias must share the cache")

				-- .lua 回退(只有 .lua 时)
				local fallback = ecs:RequireLib("fallback")
				assert(fallback.kind == "lua", ".lua fallback must resolve")

				-- 跨调用保留状态:下一段 chunk 靠它证明缓存是同一张表
				math.marker = "kept"
			)", "TestRequireLibPositive");

			RUN_OK(R"(
				local math = ecs:RequireLib("util/math")
				assert(math.marker == "kept", "cache must survive across chunks: same table, not re-executed")
			)", "TestRequireLibCrossChunkCache");

			RUN_OK(R"(
				assert(ecs:RequireLib("reloadme").n == 1, "library must load")
			)", "TestRequireLibReloadBefore");
			writeLib(libRoot / "reloadme.luau", "return { n = 2 }\n");
			RUN_OK(R"(
				local reloaded = ecs:RequireLib("reloadme")
				assert(reloaded.n == 2, "changed library content must re-execute on the next RequireLib")
			)", "TestRequireLibContentHashReload");

			RUN_OK(R"(
				local probe = ecs:RequireLib("sandbox_probe")
				assert(probe.io == nil, "io must stay nil inside a library")
				assert(probe.os == nil, "os must stay nil inside a library")
				assert(probe.require == nil, "require must stay nil inside a library")
				assert(probe.load == nil, "load must stay nil inside a library")
				assert(probe.dofile == nil, "dofile must stay nil inside a library")
				assert(probe.loadstring == nil, "loadstring must stay nil inside a library")
				assert(probe.loadfile == nil, "loadfile must stay nil inside a library")
				assert(probe.package == nil, "package must stay nil inside a library")
				assert(probe.debug == nil, "debug must stay nil inside a library")
				assert(type(probe.ecsTable) == "table", "library must run under the same sandbox globals")
			)", "TestRequireLibSandboxUnchanged");

			RUN_OK(R"(
				local function mustFail(name, label, needle)
					local ok, err = pcall(function() return ecs:RequireLib(name) end)
					assert(not ok, label .. " must fail")
					assert(type(err) == "string" and #err > 0, label .. " must have a readable error")
					if needle then
						assert(string.find(err, needle, 1, true) ~= nil, label .. " error must mention '" .. needle .. "': " .. err)
					end
					return err
				end

				mustFail("../outside", "escape path", "../outside")
				mustFail("/abs/math", "absolute path")
				mustFail("C:/math", "drive letter")
				mustFail("util\\math", "backslash escape", "backslash")
				mustFail("util/math.txt", "non-luau/lua extension", ".luau")
				local missing = mustFail("does/not/exist", "missing library", "does/not/exist")
				assert(string.find(missing, "scripts/lib/", 1, true) ~= nil, "missing error must name the attempted location")
				mustFail("adirlib", "directory instead of file", "adirlib")
				mustFail("cycle_a", "circular dependency", "circular")

				-- 循环报错后不许把"加载中"状态留在栈里:再次调用仍是同一个可读错误
				mustFail("cycle_a", "circular dependency after failure", "circular")
			)", "TestRequireLibRejectionsAndCycle");

			RUN_OK(R"(
				local okBroken, brokenErr = pcall(function() return ecs:RequireLib("broken") end)
				assert(not okBroken, "a broken library must fail")
				assert(type(brokenErr) == "string" and #brokenErr > 0, "broken library error must be readable")
			)", "TestRequireLibFailureNotCachedBefore");
			writeLib(libRoot / "broken.luau", "return { fixed = true }\n");
			RUN_OK(R"(
				local fixed = ecs:RequireLib("broken")
				assert(fixed.fixed == true, "fixing the file must let the same session load it: failures are not cached")
			)", "TestRequireLibFailureNotCachedAfter");
		}

		// =====================================================================
		// P5:物理事件进 Lua 面 —— ecs:OnContact / ecs:OnTrigger / ecs:Off
		//   场景由 C++ 搭(C++ 才是建组件的地方),Lua 只订阅;事件在固定步产出、
		//   由脚本层帧系统在 Late 阶段派发。
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			Entity ground = scene.CreateEntityShell("Ground");
			ground.AddComponent<TransformComponent>(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
			RigidBody2DComponent& groundBody = ground.AddComponent<RigidBody2DComponent>();
			groundBody.Type = RigidBody2DComponent::BodyType::Static;
			BoxCollider2DComponent& groundCollider = ground.AddComponent<BoxCollider2DComponent>();
			groundCollider.Size = { 2.0f, 0.5f };
			groundCollider.Restitution = 0.0f;

			Entity sensor = scene.CreateEntityShell("Sensor");
			sensor.AddComponent<TransformComponent>(glm::vec3(0.0f, 1.5f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
			RigidBody2DComponent& sensorBody = sensor.AddComponent<RigidBody2DComponent>();
			sensorBody.Type = RigidBody2DComponent::BodyType::Static;
			BoxCollider2DComponent& sensorCollider = sensor.AddComponent<BoxCollider2DComponent>();
			sensorCollider.Size = { 1.0f, 0.1f };
			sensorCollider.IsSensor = true;

			Entity faller = scene.CreateEntityShell("Faller");
			faller.AddComponent<TransformComponent>(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
			RigidBody2DComponent& fallerBody = faller.AddComponent<RigidBody2DComponent>();
			fallerBody.Type = RigidBody2DComponent::BodyType::Dynamic;
			BoxCollider2DComponent& fallerCollider = faller.AddComponent<BoxCollider2DComponent>();
			fallerCollider.Size = { 0.25f, 0.25f };
			fallerCollider.Restitution = 0.0f;

			scene.OnRuntimeStart();

			RUN_OK(R"(
				physicsTriggerHits = 0
				physicsContactHits = 0
				physicsTriggerPersist = 0
				physicsHandlesAreNumbers =
					type(ecs.OnContact) == "function" and type(ecs.OnTrigger) == "function"

				triggerHandle = ecs:OnTrigger(function(e)
					physicsTriggerHits = physicsTriggerHits + 1
					if e.phase == "Persist" then physicsTriggerPersist = physicsTriggerPersist + 1 end
					assert(e.sensor ~= nil and e.other ~= nil, "trigger event must carry sensor + other")
					assert(e.phase == "Begin" or e.phase == "Persist" or e.phase == "End", "unknown trigger phase")
				end)
				contactHandle = ecs:OnContact(function(e)
					physicsContactHits = physicsContactHits + 1
					assert(e.a ~= nil and e.b ~= nil, "contact event must carry both entities")
					assert(type(e.point) == "table" and type(e.normal) == "table", "contact must carry geometry tables")
					assert(type(e.depth) == "number", "contact must carry penetration depth")
				end)
				assert(type(triggerHandle) == "number" and triggerHandle > 0, "OnTrigger must return a handle")
				assert(type(contactHandle) == "number" and contactHandle > 0, "OnContact must return a handle")
			)", "TestPhysicsEventSubscribe");

			// 每帧:固定步产出事件 → 可变阶段(Late)由脚本层帧系统派发。
			for (int frame = 0; frame < 180; ++frame)
			{
				scene.OnFixedUpdate(Timestep(1.0f / 60.0f));
				scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
			}

			RUN_OK(R"(
				assert(physicsHandlesAreNumbers, "OnContact / OnTrigger must be exposed on the ecs table")
				assert(physicsContactHits >= 1, "landing must produce at least one contact event")
				assert(physicsTriggerHits >= 2, "passing through a sensor must produce Begin and End")
				assert(physicsTriggerPersist >= 1,
					"a sensor overlap that spans frames must produce Persist -- 2D sensor persist channel")
			)", "TestPhysicsEventsDispatched");

			// ecs:Off 必须真的注销物理事件订阅(句柄走独立区段,不能误伤组件观察者)。
			RUN_OK(R"(
				local after = 0
				local handle = ecs:OnTrigger(function(e) after = after + 1 end)
				ecs:Off(handle)
				physicsOffAccepted = true
			)", "TestPhysicsEventOff");

			for (int frame = 0; frame < 30; ++frame)
			{
				scene.OnFixedUpdate(Timestep(1.0f / 60.0f));
				scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
			}
			RUN_OK(R"(
				assert(physicsOffAccepted, "ecs:Off must accept a physics event handle")
			)", "TestPhysicsEventOffNoCrash");

			scene.OnRuntimeStop();
			ScriptEngine::SetActiveScene(nullptr);
		}

				// =====================================================================
		// 14. ECS 拓扑扩展验证: Startup / Teardown / Interval / Condition / OnChange / Zero-GC
		// =====================================================================
		{
			Scene scene(context);
			ScriptEngine::SetActiveScene(&scene);

			RUN_OK(R"(
				startupRan = 0
				teardownRan = 0
				ecs:AddStartupSystem("InitLevel", function()
					startupRan = startupRan + 1
					local e = ecs:CreateEntity("SpawnedInStartup")
					e:AddComponent(Comp.Transform)
				end)

				ecs:AddTeardownSystem("CleanupLevel", function()
					teardownRan = teardownRan + 1
				end)
			)", "TestStartupTeardownRegister");

			CHECK(!scene.IsRunning());
			scene.OnRuntimeStart();
			CHECK(scene.IsRunning());

			RUN_OK(R"(
				assert(startupRan == 1, "Startup system must run exactly once on scene start")
				assert(teardownRan == 0, "Teardown system must not run while scene is active")
			)", "TestStartupRan");

			// Interval 节流与 Condition 门禁验证
			RUN_OK(R"(
				intervalTicks = 0
				gatedTicks = 0
				gateOpen = false

				ecs:AddSystem({
					name = "IntervalSys",
					interval = 0.1, -- 10Hz
					update = function(dt)
						intervalTicks = intervalTicks + 1
					end
				})

				ecs:AddSystem({
					name = "GatedSys",
					condition = function() return gateOpen end,
					update = function(dt)
						gatedTicks = gatedTicks + 1
					end
				})
			)", "TestIntervalAndConditionRegister");

			// 推进 5 帧,每帧 0.02s (总计 0.1s -> IntervalSys 触发 1 次; GatedSys gateOpen=false 不触发)
			for (int i = 0; i < 5; ++i)
				scene.OnUpdateRuntime(Timestep(0.02f));

			RUN_OK(R"(
				assert(intervalTicks == 1, "Interval system must trigger once after 0.1s")
				assert(gatedTicks == 0, "Gated system must not run when condition is false")
				gateOpen = true
			)", "TestInterval1");

			scene.OnUpdateRuntime(Timestep(0.02f));
			RUN_OK(R"(
				assert(gatedTicks == 1, "Gated system must run when condition becomes true")
			)", "TestGated1");

			// OnChange 反应式变更监听验证 (数据写入触发观察者)
			RUN_OK(R"(
				changedHits = 0
				local changeHandle = ecs:OnChange(Comp.Transform, function(entity, t)
					changedHits = changedHits + 1
				end)
				assert(type(changeHandle) == "number", "OnChange must return observer handle")

				local query = ecs:Query({ Comp.Transform })
				query:Each(function(entity, t)
					t.Location = vec3.new(42.0, 0.0, 0.0) -- 触发字段数据写入
				end)
				assert(changedHits >= 1, "Writing field must trigger OnChange observer")
			)", "TestOnChange");

			// 零 GC 享元代理复用验证 (collectgarbage count 净增长为 0)
			RUN_OK(R"(
				local query = ecs:Query({ Comp.Transform })
				local memBefore = gcinfo()
				for iter = 1, 10 do
					query:Each(function(entity, t)
						local loc = t.Location.x
					end)
				end
				local memAfter = gcinfo()
				-- 允许微量 VM 栈临时波动 (< 1KB),绝不随实体/迭代数线性膨胀
				assert(memAfter - memBefore < 2.0, "Zero-GC flyweight pooling must prevent heap allocations in Each")
			)", "TestZeroGcEach");

			scene.OnRuntimeStop();
			CHECK(!scene.IsRunning());

			RUN_OK(R"(
				assert(teardownRan == 1, "Teardown system must run exactly once on scene stop")
			)", "TestTeardownRan");

			ScriptEngine::SetActiveScene(nullptr);
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
