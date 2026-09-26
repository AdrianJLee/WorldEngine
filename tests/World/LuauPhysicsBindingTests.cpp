// P2 W3f:2D 物理运行时 API(Entity 上的速度/冲量/力/同步,以及运行时建/移除刚体)。
//
// 覆盖:
//   1. 动态体:SetLinearVelocity/GetLinearVelocity 往返,步进后位移方向与大小符合预期(容差);
//   2. ApplyLinearImpulse / ApplyForce 对速度的影响(1 kg 盒体、重力可控);
//   3. 角速度读写;SyncPhysicsBody 把 Transform 推给 Box2D;
//   4. 没有 2D 刚体的实体调用 → 可读 Lua error(不静默);实体销毁后调用安全失败;
//   5. 运行时 AddComponent("RigidBody2DComponent") 在脚本回调内同步生效并立即建刚体,
//      速度驱动当帧可见;RemoveComponent 走既有延迟路径并销毁刚体。
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

#include <box2d/box2d.h>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
	using namespace World;

	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	bool Nearly(double actual, double expected, double tolerance)
	{
		return std::abs(actual - expected) <= tolerance;
	}

	void SetGlobal(const char* name, const std::string& value)
	{
		CHECK(ScriptEngine::GetState().SetGlobal(name, ScriptValue::String(value)));
	}

	void SetGlobalNumber(const char* name, double value)
	{
		CHECK(ScriptEngine::GetState().SetGlobal(name, ScriptValue::Number(value)));
	}

	void SetEntityGlobal(const char* name, Entity entity)
	{
		ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
		const ScriptValue value = bindings.NewUserdata("Entity");
		Entity* target = nullptr;
		CHECK(bindings.Unwrap<Entity>("Entity", value, &target) && target != nullptr);
		new (target) Entity(entity);
		CHECK(ScriptEngine::GetState().SetGlobal(name, value));
	}

	struct ProbeReport
	{
		std::string Tag;
		double X = 0.0;
		double Y = 0.0;
		double VelocityX = 0.0;
		double VelocityY = 0.0;
		int Count = 0;
		bool Seen = false;
	};

	ProbeReport s_Report;

	ScriptValue ReportToHarness(const ScriptValue* args, std::size_t count)
	{
		if (count < 5)
			throw std::runtime_error("TEST_Physics2DReport expects (tag, x, y, vx, vy)");
		std::string tag;
		double x = 0.0, y = 0.0, vx = 0.0, vy = 0.0;
		if (!args[0].AsString(&tag) || !args[1].AsNumber(&x) || !args[2].AsNumber(&y) ||
			!args[3].AsNumber(&vx) || !args[4].AsNumber(&vy))
			throw std::runtime_error("TEST_Physics2DReport received a non-number argument");
		s_Report.Tag = tag;
		s_Report.X = x;
		s_Report.Y = y;
		s_Report.VelocityX = vx;
		s_Report.VelocityY = vy;
		s_Report.Count += 1;
		s_Report.Seen = true;
		return ScriptValue::Nil();
	}

	constexpr const char* kPhysicsProbePath = "scripts/tests/Physics2DProbe.lua";

	struct Fixture
	{
		Scene World { TestContext() };

		Fixture()
		{
			s_Report = ProbeReport {};
			ScriptBindingContext& bindings = ScriptEngine::GetBindingContext();
			CHECK(ScriptEngine::GetState().SetGlobal("TEST_Physics2DReport",
				bindings.CreateFunction("TEST_Physics2DReport", &ReportToHarness)));
		}

		Entity AddProbeBody(const char* tag, RigidBody2DComponent::BodyType type, const glm::vec3& location,
			const char* mode, bool collider = true)
		{
			Entity entity = Entity::CreateEntity(&World, tag);
			TransformComponent& transform = entity.AddComponent<TransformComponent>();
			transform.SetLocation(location);
			RigidBody2DComponent& body = entity.AddComponent<RigidBody2DComponent>();
			body.Type = type;
			if (collider)
				entity.AddComponent<BoxCollider2DComponent>();
			entity.AddComponent<LuauScriptComponent>(kPhysicsProbePath);
			SetGlobal("TEST_Physics2DMode", mode);
			return entity;
		}

		Entity AddProbeOnly(const char* tag, const char* mode)
		{
			Entity entity = Entity::CreateEntity(&World, tag);
			entity.AddComponent<TransformComponent>();
			entity.AddComponent<LuauScriptComponent>(kPhysicsProbePath);
			SetGlobal("TEST_Physics2DMode", mode);
			return entity;
		}

		void Start()
		{
			World.OnRuntimeStart();
		}

		void Step(int index)
		{
			SetGlobalNumber("TEST_Physics2DPhase", index);
			s_Report.Seen = false;
			World.OnScriptUpdate(Timestep(0.5f));
			CHECK(CheckProbeScript());
		}

		// 探针脚本出错会置 Faulted 并把错误写进 LastError:测试里直接失败,避免"静默通过"。
		bool CheckProbeScript()
		{
			for (const entt::entity handle : static_cast<const Scene&>(World).GetRegistry().view<LuauScriptComponent>())
			{
				const LuauScriptComponent& script = static_cast<const Scene&>(World).GetRegistry().get<LuauScriptComponent>(handle);
				if (!script.Runtime.LastError.empty())
				{
					std::fprintf(stderr, "Physics2DProbe.lua failed: %s\n", script.Runtime.LastError.c_str());
					return false;
				}
			}
			return true;
		}

		void Stop()
		{
			World.OnRuntimeStop();
		}
	};

	// 1.速度往返 + 步进位移(重力 y=-9.8,1 kg 盒体,0.5 s 一步)。
	void VelocityDrivesDynamicBody()
	{
		// 纯 C++ 对照:一步自由落体的位移只由重力决定(gravity y=-9.8、dt=0.5 → dy≈-1.53)。
		{
			Fixture control;
			Entity faller = control.AddProbeBody("control faller", RigidBody2DComponent::BodyType::Dynamic,
				glm::vec3(0.0f, 10.0f, 0.0f), "none");
			control.Start();
			const b2Vec2 before = b2Body_GetPosition(faller.GetComponent<RigidBody2DComponent>().RuntimeBodyId);
			control.Step(1);
			const b2Vec2 after = b2Body_GetPosition(faller.GetComponent<RigidBody2DComponent>().RuntimeBodyId);
			CHECK(Nearly(after.y - before.y, -1.5313, 0.03));
			CHECK(Nearly(b2Body_GetLinearVelocity(faller.GetComponent<RigidBody2DComponent>().RuntimeBodyId).y, -4.9, 0.05));
			control.Stop();
		}
		Fixture fixture;
		Entity entity = fixture.AddProbeBody("dynamic velocity", RigidBody2DComponent::BodyType::Dynamic,
			glm::vec3(0.0f, 10.0f, 0.0f), "drive");
		fixture.Start();
		auto& body = entity.GetComponent<RigidBody2DComponent>();
		CHECK(b2Body_IsValid(body.RuntimeBodyId));
		CHECK(b2Body_GetType(body.RuntimeBodyId) == b2_dynamicBody);

		b2Body_SetLinearVelocity(body.RuntimeBodyId, { 3.0f, 0.0f });
		CHECK(b2Body_GetLinearVelocity(body.RuntimeBodyId).x > 2.999f);

		fixture.Step(1);
		CHECK(s_Report.Seen && s_Report.Tag == "start");
		// 报告发生在物理步进之后(与 C++ 侧完全一致):0.5 s 一步已产生水平位移与自由落体。
		CHECK(Nearly(s_Report.X, 1.5, 0.01));
		CHECK(Nearly(s_Report.Y, 10.0 - 1.5313, 0.02));
		CHECK(Nearly(s_Report.VelocityX, 3.0, 0.001));
		CHECK(Nearly(s_Report.VelocityY, -4.9, 0.01));
		CHECK(Nearly(s_Report.X, b2Body_GetPosition(body.RuntimeBodyId).x, 0.001));
		CHECK(Nearly(s_Report.Y, b2Body_GetPosition(body.RuntimeBodyId).y, 0.001));
		const b2Vec2 after = b2Body_GetPosition(body.RuntimeBodyId);
		CHECK(Nearly(after.x, 1.5, 0.01));   // 3 m/s × 0.5 s

		fixture.Step(2);
		CHECK(s_Report.Seen && s_Report.Tag == "end");
		CHECK(s_Report.X > 2.9 && s_Report.X < 3.1);
		CHECK(Nearly(s_Report.VelocityX, 3.0, 0.001));
		fixture.Stop();
	}

	// 2.冲量:1 kg 动态体施加 1 N·s → 速度 1 m/s;力:向上 10 N 抵消重力后速度归零。
	void ImpulseAndForceChangeVelocity()
	{
		// 纯 C++ 对照:同样的动态盒体,C++ 设速度 1 m/s,一步位移必须≈0.5。
		{
			Fixture probe;
			Entity control = probe.AddProbeBody("control velocity", RigidBody2DComponent::BodyType::Dynamic,
				glm::vec3(0.0f, 10.0f, 0.0f), "none");
			probe.Start();
			auto id = [&control]() { return control.GetComponent<RigidBody2DComponent>().RuntimeBodyId; };
			const b2Vec2 before = b2Body_GetPosition(id());
			b2Body_SetLinearVelocity(id(), { 1.0f, 0.0f });
			probe.Step(1);
			const b2Vec2 after = b2Body_GetPosition(id());
			CHECK(Nearly(after.x - before.x, 0.5, 0.02));
			CHECK(Nearly(after.y - before.y, -1.5313, 0.03));
			CHECK(Nearly(b2Body_GetLinearVelocity(id()).x, 1.0, 0.01));
			probe.Stop();
		}
		Fixture fixture;
		Entity entity = fixture.AddProbeBody("dynamic impulse", RigidBody2DComponent::BodyType::Dynamic,
			glm::vec3(-10.0f, 10.0f, 0.0f), "impulse");
		fixture.Start();
		auto bodyId = [&entity]() { return entity.GetComponent<RigidBody2DComponent>().RuntimeBodyId; };
		CHECK(b2Body_IsValid(bodyId()));
		CHECK(b2Body_GetType(bodyId()) == b2_dynamicBody);

		// 第 1 步:脚本本帧不动手,只验证重力把静止体带起来(质量 1 kg → vy = -g·dt)。
		fixture.Step(1);
		const b2Vec2 afterFall = b2Body_GetPosition(bodyId());
		CHECK(Nearly(b2Body_GetLinearVelocity(bodyId()).y, -4.9, 0.05));
		CHECK(Nearly(afterFall.y, 10.0 - 1.5313, 0.03));

		// 第 2 步:脚本清速 + 施加 1 N·s 冲量(1 kg → 1 m/s),同一步继续自由落体。
		fixture.Step(2);
		// 脚本在步进前清速并施加 1 N·s 冲量:1 kg 的盒体得到 1 m/s,同时在重力下自由落体。
		// 冲量在回调内(步进之后)施加:水平位移在下一步体现,期间仍受重力。
		CHECK(Nearly(b2Body_GetLinearVelocity(bodyId()).x, 1.0, 0.01));

		// 第 3 步:不再改速度,验证 1 m/s 的水平位移(0.5 m/步)与重力加速度。
		const b2Vec2 beforeStep3 = b2Body_GetPosition(bodyId());
		fixture.Step(3);
		const b2Vec2 afterStep3 = b2Body_GetPosition(bodyId());
		CHECK(Nearly(afterStep3.x - beforeStep3.x, 0.5, 0.02));
		CHECK(Nearly(afterStep3.y - beforeStep3.y, -1.5313, 0.03));
		CHECK(Nearly(b2Body_GetLinearVelocity(bodyId()).x, 1.0, 0.01));
		CHECK(Nearly(b2Body_GetLinearVelocity(bodyId()).y, -4.9, 0.05));

		SetGlobal("TEST_Physics2DMode", "force");
		fixture.Step(4);
		// 施加前脚本次先把速度清零,这一次步进的净加速度为 0(重力 9.8 + 力 10/1)。
		CHECK(Nearly(b2Body_GetLinearVelocity(bodyId()).x, 0.0, 0.001));
		CHECK(Nearly(b2Body_GetLinearVelocity(bodyId()).y, 0.0, 0.05));
		fixture.Stop();
	}

	// 3.角速度与 SyncPhysicsBody(运动学/静态把 Transform 推给 Box2D)。
	void AngularVelocityAndTransformSync()
	{
		Fixture fixture;
		Entity entity = fixture.AddProbeBody("kinematic sync", RigidBody2DComponent::BodyType::Kinematic,
			glm::vec3(0.0f, 0.0f, 0.0f), "angular", false);
		fixture.Start();
		auto bodyId = [&entity]() { return entity.GetComponent<RigidBody2DComponent>().RuntimeBodyId; };

		fixture.Step(1);
		CHECK(Nearly(b2Body_GetAngularVelocity(bodyId()), 2.0, 0.001));

		SetGlobal("TEST_Physics2DMode", "sync");
		fixture.Step(2);
		const b2Vec2 position = b2Body_GetPosition(bodyId());
		CHECK(Nearly(position.x, 7.0, 0.001) && Nearly(position.y, 11.0, 0.001));
		fixture.Stop();
	}

	// 4.没有刚体的实体:脚本侧可读错误;实体销毁后调用安全失败。
	void MissingBodyAndDestroyedEntityFailSafely()
	{
		Fixture fixture;
		Entity noBody = fixture.AddProbeOnly("no body", "no_body");
		Entity noBodyImpulse = fixture.AddProbeOnly("no body impulse", "apply_no_body");
		// 停止态先建被销毁的实体:运行时(活动场景)的组件新增只能走回调/白名单，
		// 这里用"启动前配置 + 运行时延迟销毁"覆盖同一生命周期语义。
		Entity doomed = Entity::CreateEntity(&fixture.World, "doomed body");
		doomed.AddComponent<TransformComponent>();
		doomed.AddComponent<RigidBody2DComponent>().Type = RigidBody2DComponent::BodyType::Dynamic;
		doomed.AddComponent<BoxCollider2DComponent>();
		const uint32_t doomedRaw = static_cast<uint32_t>(doomed);
		fixture.Start();
		fixture.Step(1);
		CHECK(doomed.IsValid());
		CHECK(doomed.HasComponent<RigidBody2DComponent>());
		CHECK(doomed.HasComponent<BoxCollider2DComponent>());
		SetEntityGlobal("TEST_Physics2DDoomed", doomed);
		// 每次重新取组件:entt 的存储搬迁会让之前拿到的组件引用失效。
		const b2BodyId doomedHandle = doomed.GetComponent<RigidBody2DComponent>().RuntimeBodyId;
		CHECK(b2Body_IsValid(doomedHandle));
		(void)doomedRaw;
		CHECK(fixture.World.DeferStructuralChange([&doomed](Scene& scene) mutable
			{
				if (doomed.IsValid()) Entity::DestroyEntity(&scene, doomed);
			}));
		fixture.Step(2);
		CHECK(!doomed.IsValid());
		CHECK(!b2Body_IsValid(doomedHandle));
		std::string error;
		ScriptEngine::GetState().RunString(
			"local ok, err = pcall(function() return TEST_Physics2DDoomed:GetLinearVelocity() end) "
			"assert(not ok, 'destroyed entity must raise') "
			"assert(string.find(err, 'requires a live entity', 1, true), err)",
			"physics_destroyed_entity", &error);
		CHECK(error.empty());

		(void)noBody;
		(void)noBodyImpulse;
		fixture.Stop();
		CHECK(ScriptEngine::GetState().ClearGlobal("TEST_Physics2DDoomed"));
	}

	// 5.运行时 Add/RemoveComponent:脚本回调内同步建刚体(当帧可用),移除走延迟路径并销毁刚体。
	void RuntimeAddAndRemoveBody()
	{
		Fixture fixture;
		Entity entity = Entity::CreateEntity(&fixture.World, "runtime body");
		TransformComponent& transform = entity.AddComponent<TransformComponent>();
		transform.SetLocation(glm::vec3(0.0f, 1.0f, 0.0f));
		entity.AddComponent<LuauScriptComponent>(kPhysicsProbePath);
		SetGlobal("TEST_Physics2DMode", "add_body");
		fixture.Start();
		CHECK(!entity.HasComponent<RigidBody2DComponent>());

		fixture.Step(1);
		CHECK(s_Report.Seen && s_Report.Tag == "added");
		CHECK(entity.HasComponent<RigidBody2DComponent>());
		CHECK(entity.HasComponent<BoxCollider2DComponent>());
		b2BodyId bodyHandle = entity.GetComponent<RigidBody2DComponent>().RuntimeBodyId;
		CHECK(b2Body_IsValid(bodyHandle));
		// 当帧:AddComponent 之后立刻可以设速度(不静默、不延迟)。
		CHECK(Nearly(b2Body_GetLinearVelocity(bodyHandle).y, 1.0, 0.001));

		fixture.Step(2);
		CHECK(s_Report.Seen && s_Report.Tag == "moved");
		CHECK(s_Report.Y > 1.2);   // 1 m/s 经一步 0.5 s 已经明显上升

		SetGlobal("TEST_Physics2DMode", "remove_body");
		fixture.Step(3);
		CHECK(s_Report.Count == 2);   // 本步没有新报告
		CHECK(!entity.HasComponent<RigidBody2DComponent>());   // 延迟路径在步末提交
		// 刚体随组件移除销毁:句柄失效,且脚本侧调用重新报可读错误。
		CHECK(!b2Body_IsValid(bodyHandle));

		SetGlobal("TEST_Physics2DMode", "destroyed");
		Entity::DestroyEntity(&fixture.World, entity);
		fixture.Step(4);
		CHECK(!entity.IsValid());
		fixture.Stop();
	}
}

int main()
{
	try
	{
		setvbuf(stdout, nullptr, _IONBF, 0);
		World::Log::Init();
		World::ScriptEngine::Init();

		const std::pair<const char*, void(*)()> tests[] = {
			{ "linear velocity drives a dynamic body", VelocityDrivesDynamicBody },
			{ "impulse and force change velocity", ImpulseAndForceChangeVelocity },
			{ "angular velocity and transform sync", AngularVelocityAndTransformSync },
			{ "missing body and destroyed entity fail safely", MissingBodyAndDestroyedEntityFailSafely },
			{ "runtime add/remove rigid body", RuntimeAddAndRemoveBody },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		World::ScriptEngine::Shutdown();
		if (failures == 0)
		{
			std::printf("World.LuauPhysicsBinding: all checks passed\n");
			return 0;
		}
		std::fprintf(stderr, "World.LuauPhysicsBinding: %d group(s) failed\n", failures);
		return 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Test setup failed: %s\n", error.what());
		if (World::ScriptEngine::IsInitialized()) World::ScriptEngine::Shutdown();
		return 1;
	}
	catch (...)
	{
		std::fprintf(stderr, "Test setup failed: unknown exception\n");
		if (World::ScriptEngine::IsInitialized()) World::ScriptEngine::Shutdown();
		return 1;
	}
}
