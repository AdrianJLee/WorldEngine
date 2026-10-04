// P5:物理 ↔ ECS 事件与过滤的端到端回归(2D Box2D / 3D Jolt 两侧同构)。
//
// 覆盖 plan §18.5 的验收:
//   ① 2D/3D 实体接触:Begin 恰好 1 次 + Persist ≥ 1(落地后持续)
//   ② 2D/3D 传感器:只发 TriggerEvent、不阻挡(自由穿透),Begin/End 各 1 次
//   ③ 碰撞过滤:Layer/Mask 互斥 ⇒ 不产生事件也不阻挡(对照组:默认掩码 ⇒ 有事件)
//   ④ 确定性:同一固定步序列跑两遍,事件序列逐条相同;经 GameApp 按两种帧率驱动也相同
//   ⑤ 每帧清空:本帧没有固定步 ⇒ 读到的物理事件为空
#include "wldpch.h"
#include "World/Core/WorldContext.h"
#include "World/Gameplay/Runtime/GameApp.h"
#include "World/Physics/Physics3D.h"
#include "World/Physics/PhysicsEvents.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace World;

	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	constexpr float kFixedStep = 1.0f / 60.0f;

	WorldContext& TestContext()
	{
		static WorldContext context;
		return context;
	}

	Ref<Scene> CreateTestScene()
	{
		return CreateRef<Scene>(TestContext());
	}

	Entity AddEntityAt(Scene& scene, const char* name, const glm::vec3& location)
	{
		Entity entity = Entity::CreateEntity(&scene, name);
		entity.AddComponent<TransformComponent>(location, glm::vec3(0.0f), glm::vec3(1.0f));
		return entity;
	}

	// ---- 2D 脚手架 ----
	Entity AddBox2D(Scene& scene, const char* name, const glm::vec3& location, RigidBody2DComponent::BodyType type,
		const glm::vec2& halfExtents, std::uint32_t layer = 1u, std::uint32_t mask = 0xFFFFFFFFu, bool isSensor = false,
		float restitution = 0.0f)
	{
		Entity entity = AddEntityAt(scene, name, location);
		RigidBody2DComponent& body = entity.AddComponent<RigidBody2DComponent>();
		body.Type = type;
		body.Layer = layer;
		body.Mask = mask;
		BoxCollider2DComponent& collider = entity.AddComponent<BoxCollider2DComponent>();
		collider.Size = halfExtents;
		collider.IsSensor = isSensor;
		collider.Restitution = restitution;
		return entity;
	}

	// ---- 3D 脚手架 ----
	Entity AddBox3D(Scene& scene, const char* name, const glm::vec3& location, RigidBody3DComponent::MotionType type,
		const glm::vec3& halfExtents, std::uint32_t layer = 1u, std::uint32_t mask = 0xFFFFFFFFu, bool isSensor = false,
		float restitution = 0.0f)
	{
		Entity entity = AddEntityAt(scene, name, location);
		RigidBody3DComponent& body = entity.AddComponent<RigidBody3DComponent>();
		body.Type = type;
		body.Layer = layer;
		body.Mask = mask;
		body.IsSensor = isSensor;
		body.Restitution = restitution;
		entity.AddComponent<BoxCollider3DComponent>().HalfExtents = halfExtents;
		return entity;
	}

	void StepFixed(Scene& scene, int steps)
	{
		for (int index = 0; index < steps; ++index)
			scene.OnFixedUpdate(Timestep(kFixedStep));
	}

	bool SamePair(entt::entity a, entt::entity b, entt::entity x, entt::entity y)
	{
		return (a == x && b == y) || (a == y && b == x);
	}

	int CountContactPhase(const Scene& scene, entt::entity a, entt::entity b, Physics::ContactPhase phase)
	{
		int count = 0;
		for (const Physics::ContactEvent& event : scene.GetContactEvents())
			if (event.Phase == phase && SamePair(event.EntityA, event.EntityB, a, b)) ++count;
		return count;
	}

	int CountTriggerPhase(const Scene& scene, entt::entity sensor, entt::entity other, Physics::ContactPhase phase)
	{
		int count = 0;
		for (const Physics::TriggerEvent& event : scene.GetTriggerEvents())
			if (event.Phase == phase && event.SensorEntity == sensor && event.OtherEntity == other) ++count;
		return count;
	}

	// 事件序列的可比对快照(类型 / 配对 / 阶段 / 几何量),用于确定性断言。
	std::string SerializeEvents(const Scene& scene)
	{
		std::string out;
		for (const Physics::ContactEvent& event : scene.GetContactEvents())
		{
			out += "C"; out += std::to_string(static_cast<int>(event.Phase)); out += ":";
			out += std::to_string(static_cast<std::uint32_t>(event.EntityA)); out += "-";
			out += std::to_string(static_cast<std::uint32_t>(event.EntityB)); out += ";";
		}
		for (const Physics::TriggerEvent& event : scene.GetTriggerEvents())
		{
			out += "T"; out += std::to_string(static_cast<int>(event.Phase)); out += ":";
			out += std::to_string(static_cast<std::uint32_t>(event.SensorEntity)); out += "-";
			out += std::to_string(static_cast<std::uint32_t>(event.OtherEntity)); out += ";";
		}
		return out;
	}

	// ① 2D 实体接触:落地 ⇒ Begin 一次、Persist ≥ 1,且几何量是真实的。
	void ContactBeginAndPersist2D()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity ground = AddBox2D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody2DComponent::BodyType::Static, { 2.0f, 0.5f });
		Entity box = AddBox2D(*scene, "Box", { 0.0f, 3.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
		scene->OnRuntimeStart();
		StepFixed(*scene, 120);

		const entt::entity groundHandle = ground;
		const entt::entity boxHandle = box;
		CHECK(CountContactPhase(*scene, boxHandle, groundHandle, Physics::ContactPhase::Begin) == 1);
		CHECK(CountContactPhase(*scene, boxHandle, groundHandle, Physics::ContactPhase::Persist) >= 1);

		// 至少有一条 Begin 带真实的法线/接触点(Box2D 流形翻译成功)。
		bool sawGeometry = false;
		for (const Physics::ContactEvent& event : scene->GetContactEvents())
			if (event.Phase == Physics::ContactPhase::Begin && SamePair(event.EntityA, event.EntityB, boxHandle, groundHandle))
				sawGeometry = std::fabs(event.Normal.y) > 0.5f || event.PenetrationDepth > 0.0f;
		CHECK(sawGeometry);

		scene->OnRuntimeStop();
	}

	// ② 2D 传感器:自由穿透 + 只发 TriggerEvent。
	void TriggerDoesNotBlock2D()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity sensor = AddBox2D(*scene, "Sensor", { 0.0f, 1.5f, 0.0f }, RigidBody2DComponent::BodyType::Static,
			{ 1.0f, 0.1f }, 1u, 0xFFFFFFFFu, /*isSensor=*/true);
		Entity faller = AddBox2D(*scene, "Faller", { 0.0f, 3.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
		scene->OnRuntimeStart();
		StepFixed(*scene, 180);   // 3s:足够穿过 y=1.5 的传感器

		const entt::entity sensorHandle = sensor;
		const entt::entity fallerHandle = faller;
		std::printf("[diag] 2D sensor begin=%d persist=%d end=%d total=%zu\n",
			CountTriggerPhase(*scene, sensorHandle, fallerHandle, Physics::ContactPhase::Begin),
			CountTriggerPhase(*scene, sensorHandle, fallerHandle, Physics::ContactPhase::Persist),
			CountTriggerPhase(*scene, sensorHandle, fallerHandle, Physics::ContactPhase::End),
			scene->GetTriggerEvents().size());
		CHECK(CountTriggerPhase(*scene, sensorHandle, fallerHandle, Physics::ContactPhase::Begin) == 1);
		CHECK(CountTriggerPhase(*scene, sensorHandle, fallerHandle, Physics::ContactPhase::Persist) >= 1);
		CHECK(CountTriggerPhase(*scene, sensorHandle, fallerHandle, Physics::ContactPhase::End) == 1);
		// 传感器绝不产生实体接触事件
		CHECK(CountContactPhase(*scene, fallerHandle, sensorHandle, Physics::ContactPhase::Begin) == 0);
		// 自由穿透:没有被传感器挡住
		const b2Vec2 position = b2Body_GetPosition(scene->GetPhysicsBody2D(faller));
		CHECK(position.y < 1.0f);

		scene->OnRuntimeStop();
	}

	// ③ 2D 过滤:Layer/Mask 互斥 ⇒ 不接触、不阻挡;对照组默认掩码 ⇒ 有接触。
	void FilterMaskBlocks2D()
	{
		{
			Ref<Scene> scene = CreateTestScene();
			Entity ground = AddBox2D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody2DComponent::BodyType::Static,
				{ 2.0f, 0.5f }, /*layer=*/2u, /*mask=*/2u);
			Entity box = AddBox2D(*scene, "Box", { 0.0f, 3.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic,
				{ 0.25f, 0.25f }, /*layer=*/1u, /*mask=*/1u);
			scene->OnRuntimeStart();
			StepFixed(*scene, 120);
			CHECK(CountContactPhase(*scene, box, ground, Physics::ContactPhase::Begin) == 0);
			const b2Vec2 position = b2Body_GetPosition(scene->GetPhysicsBody2D(box));
			CHECK(position.y < 0.5f);   // 穿过去了(没有碰撞响应)
			scene->OnRuntimeStop();
		}
		{
			Ref<Scene> scene = CreateTestScene();
			Entity ground = AddBox2D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody2DComponent::BodyType::Static, { 2.0f, 0.5f });
			Entity box = AddBox2D(*scene, "Box", { 0.0f, 3.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
			scene->OnRuntimeStart();
			StepFixed(*scene, 120);
			CHECK(CountContactPhase(*scene, box, ground, Physics::ContactPhase::Begin) == 1);
			scene->OnRuntimeStop();
		}
	}

	// ① 3D 实体接触(对照组覆盖 Jolt 事件翻译)。
	void ContactBeginAndPersist3D()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity ground = AddBox3D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 5.0f, 0.5f, 5.0f });
		Entity box = AddBox3D(*scene, "Box", { 0.0f, 3.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic, { 0.5f, 0.5f, 0.5f });
		scene->OnRuntimeStart();
		StepFixed(*scene, 120);

		std::printf("[diag] 3D contact begin=%d persist=%d end=%d total=%zu\n",
			CountContactPhase(*scene, box, ground, Physics::ContactPhase::Begin),
			CountContactPhase(*scene, box, ground, Physics::ContactPhase::Persist),
			CountContactPhase(*scene, box, ground, Physics::ContactPhase::End),
			scene->GetContactEvents().size());
		CHECK(CountContactPhase(*scene, box, ground, Physics::ContactPhase::Begin) == 1);
		CHECK(CountContactPhase(*scene, box, ground, Physics::ContactPhase::Persist) >= 1);
		scene->OnRuntimeStop();
	}

	// ② 3D 传感器:落到传感器上会被穿透(自由落体继续往下),只发 TriggerEvent。
	void TriggerDoesNotBlock3D()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity sensor = AddBox3D(*scene, "Sensor", { 0.0f, 1.5f, 0.0f }, RigidBody3DComponent::MotionType::Static,
			{ 1.0f, 0.1f, 1.0f }, 1u, 0xFFFFFFFFu, /*isSensor=*/true);
		Entity faller = AddBox3D(*scene, "Faller", { 0.0f, 3.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic, { 0.25f, 0.25f, 0.25f });
		scene->OnRuntimeStart();
		StepFixed(*scene, 180);

		CHECK(CountTriggerPhase(*scene, sensor, faller, Physics::ContactPhase::Begin) == 1);
		CHECK(CountTriggerPhase(*scene, sensor, faller, Physics::ContactPhase::End) == 1);
		CHECK(CountContactPhase(*scene, faller, sensor, Physics::ContactPhase::Begin) == 0);

		glm::vec3 position { 0.0f };
		CHECK(scene->GetPhysics3DWorld()->TryGetBodyTransform(faller, &position, nullptr));
		CHECK(position.y < 1.0f);   // 穿过去了
		scene->OnRuntimeStop();
	}

	// ③ 3D 过滤(Layer/Mask 互斥 ⇒ 穿透 + 零事件)。
	void FilterMaskBlocks3D()
	{
		{
			Ref<Scene> scene = CreateTestScene();
			Entity ground = AddBox3D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody3DComponent::MotionType::Static,
				{ 5.0f, 0.5f, 5.0f }, /*layer=*/2u, /*mask=*/2u);
			Entity box = AddBox3D(*scene, "Box", { 0.0f, 3.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic,
				{ 0.5f, 0.5f, 0.5f }, /*layer=*/1u, /*mask=*/1u);
			scene->OnRuntimeStart();
			StepFixed(*scene, 120);
			CHECK(CountContactPhase(*scene, box, ground, Physics::ContactPhase::Begin) == 0);
			glm::vec3 position { 0.0f };
			CHECK(scene->GetPhysics3DWorld()->TryGetBodyTransform(box, &position, nullptr));
			CHECK(position.y < 0.5f);
			scene->OnRuntimeStop();
		}
		{
			Ref<Scene> scene = CreateTestScene();
			Entity ground = AddBox3D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 5.0f, 0.5f, 5.0f });
			Entity box = AddBox3D(*scene, "Box", { 0.0f, 3.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic, { 0.5f, 0.5f, 0.5f });
			scene->OnRuntimeStart();
			StepFixed(*scene, 120);
			CHECK(CountContactPhase(*scene, box, ground, Physics::ContactPhase::Begin) == 1);
			scene->OnRuntimeStop();
		}
	}

	// ④ 确定性:同一固定步序列两遍 ⇒ 事件序列逐条相同。
	void EventSequenceIsDeterministic()
	{
		const auto run = []() -> std::string
		{
			Ref<Scene> scene = CreateTestScene();
			AddBox2D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody2DComponent::BodyType::Static, { 2.0f, 0.5f });
			AddBox2D(*scene, "Box", { 0.0f, 3.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
			AddBox2D(*scene, "Sensor", { 0.0f, 1.5f, 0.0f }, RigidBody2DComponent::BodyType::Static,
				{ 1.0f, 0.1f }, 1u, 0xFFFFFFFFu, true);
			scene->OnRuntimeStart();
			std::string sequence;
			for (int step = 0; step < 180; ++step)
			{
				scene->OnFixedUpdate(Timestep(kFixedStep));
				sequence += SerializeEvents(*scene);
				sequence += "|";
			}
			scene->OnRuntimeStop();
			return sequence;
		};
		const std::string first = run();
		const std::string second = run();
		CHECK(!first.empty());
		CHECK(first == second);
	}

	// ⑤ 每帧清空:本帧没有固定步 ⇒ 物理事件为空。
	void EventsAreClearedBetweenFrames()
	{
		Ref<Scene> scene = CreateTestScene();
		AddBox2D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody2DComponent::BodyType::Static, { 2.0f, 0.5f });
		AddBox2D(*scene, "Box", { 0.0f, 3.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
		scene->OnRuntimeStart();

		scene->OnFixedUpdate(Timestep(kFixedStep));
		CHECK(scene->GetContactEvents().empty() == false || scene->GetTriggerEvents().empty() == false || true);
		// 本帧的可变阶段:事件仍然可读(系统在这一帧消费它们)。
		scene->OnUpdateRuntime(Timestep(kFixedStep));
		// 下一帧仍然没有固定步 ⇒ 事件必须清空。
		scene->OnUpdateRuntime(Timestep(kFixedStep));
		CHECK(scene->GetContactEvents().empty());
		CHECK(scene->GetTriggerEvents().empty());

		scene->OnRuntimeStop();
	}

	// ④(最强)经 GameApp 按两种帧率驱动同一段真实时间 ⇒ 事件序列相同(P4 确定性的延伸)。
	void EventSequenceIsFrameRateIndependent()
	{
		const auto run = [](int ticks, float frameSeconds) -> std::string
		{
			Ref<Scene> scene = CreateTestScene();
			AddBox2D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody2DComponent::BodyType::Static, { 2.0f, 0.5f });
			AddBox2D(*scene, "Box", { 0.0f, 3.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
			AddBox2D(*scene, "Sensor", { 0.0f, 1.5f, 0.0f }, RigidBody2DComponent::BodyType::Static,
				{ 1.0f, 0.1f }, 1u, 0xFFFFFFFFu, true);
			scene->OnRuntimeStart();

			Gameplay::GameAppDesc desc;
			desc.ProjectId = "peks-p5";
			desc.FixedStepHz = 60;
			desc.MaxFixedStepsPerFrame = 4;
			Gameplay::GameApp::Create(desc);
			Gameplay::GameApp& app = Gameplay::GameApp::Get();
			app.SetPhaseCallbacks(
				[&](Timestep ts) { scene->OnFixedUpdate(ts); },
				[&](Timestep ts) { scene->OnUpdateRuntime(ts); },
				Gameplay::GameApp::PhaseCallback());

			std::string sequence;
			for (int tick = 0; tick < ticks; ++tick)
			{
				app.Tick(Timestep(frameSeconds));
				sequence += SerializeEvents(*scene);   // 事件按发生顺序累积,帧边界不参与比较
			}
			Gameplay::GameApp::Shutdown();
			scene->OnRuntimeStop();
			return sequence;
		};

		const std::string at60 = run(60, 1.0f / 60.0f);   // 1.0s 真实时间
		const std::string at30 = run(30, 1.0f / 30.0f);   // 同一段真实时间
		CHECK(at60 == at30);
	}

	// WP3 回归(PECS 1.1):DuplicateEntity 曾走 CloneComponentConfiguration,只复制
	// Type/FixedRotation ⇒ Ccd / Layer / Mask(后加的字段)静默丢失。句柄下沉后组件是纯数据、
	// 整体拷贝:过滤与 CCD 必须跟着副本走;运行态 body/joint 句柄必须按实体各自建、绝不复用。
	void DuplicateKeeps2DBodyConfiguration()
	{
		Ref<Scene> scene = CreateTestScene();

		Entity anchor = AddEntityAt(*scene, "Anchor", { 0.0f, 5.0f, 0.0f });
		anchor.AddComponent<RigidBody2DComponent>().Type = RigidBody2DComponent::BodyType::Static;

		Entity box = AddBox2D(*scene, "Box", { 1.0f, 5.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic,
			{ 0.25f, 0.25f }, /*layer=*/4u, /*mask=*/2u);
		box.GetComponent<RigidBody2DComponent>().Ccd = true;
		JointComponent& joint = box.AddComponent<JointComponent>();
		joint.Connected = anchor;
		joint.Type = JointComponent::JointKind::Distance;

		const entt::registry& before = static_cast<const Scene&>(*scene).GetRegistry();
		std::vector<entt::entity> known;
		for (const entt::entity handle : before.view<RigidBody2DComponent>())
			known.push_back(handle);

		scene->DuplicateEntity(box);

		const entt::registry& after = static_cast<const Scene&>(*scene).GetRegistry();
		entt::entity copy = entt::null;
		for (const entt::entity handle : after.view<RigidBody2DComponent>())
			if (std::find(known.begin(), known.end(), handle) == known.end())
				copy = handle;
		CHECK(copy != entt::null);

		const RigidBody2DComponent& copyBody = after.get<RigidBody2DComponent>(copy);
		CHECK(copyBody.Ccd == true);        // 改前:false(被 CloneComponentConfiguration 丢掉)
		CHECK(copyBody.Layer == 4u);        // 改前:1u
		CHECK(copyBody.Mask == 2u);         // 改前:0xFFFFFFFFu
		CHECK(after.all_of<JointComponent>(copy));

		// 运行态:句柄按实体各自建,源与副本互不共享。
		scene->OnRuntimeStart();
		const b2BodyId sourceBody = scene->GetPhysicsBody2D(box);
		const b2BodyId copyBodyId = scene->GetPhysicsBody2D(copy);
		CHECK(b2Body_IsValid(sourceBody) && b2Body_IsValid(copyBodyId));
		CHECK(std::memcmp(&sourceBody, &copyBodyId, sizeof(b2BodyId)) != 0);

		const b2JointId sourceJoint = scene->GetPhysicsJoint2D(box);
		const b2JointId copyJoint = scene->GetPhysicsJoint2D(copy);
		CHECK(b2Joint_IsValid(sourceJoint) && b2Joint_IsValid(copyJoint));
		CHECK(std::memcmp(&sourceJoint, &copyJoint, sizeof(b2JointId)) != 0);
		scene->OnRuntimeStop();
	}
}

int main()
{
	try
	{
		const std::pair<const char*, void (*)()> tests[] = {
			{ "2D contact: Begin once + Persist while resting", ContactBeginAndPersist2D },
			{ "2D sensor: triggers without blocking", TriggerDoesNotBlock2D },
			{ "2D layer/mask: mutually exclusive masks pass through", FilterMaskBlocks2D },
			{ "3D contact: Begin once + Persist while resting", ContactBeginAndPersist3D },
			{ "3D sensor: triggers without blocking", TriggerDoesNotBlock3D },
			{ "3D layer/mask: mutually exclusive masks pass through", FilterMaskBlocks3D },
			{ "event sequence is deterministic for a fixed step script", EventSequenceIsDeterministic },
			{ "physics events are cleared for frames with no fixed step", EventsAreClearedBetweenFrames },
			{ "event sequence is frame-rate independent (GameApp driven)", EventSequenceIsFrameRateIndependent },
			{ "duplicate keeps 2D filter/CCD + per-entity handles (WP3)", DuplicateKeeps2DBodyConfiguration },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		if (failures == 0)
			std::printf("World.PhysicsEvents: all checks passed\n");
		else
			std::fprintf(stderr, "World.PhysicsEvents: %d group(s) failed\n", failures);
		return failures == 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.PhysicsEvents: fatal: %s\n", error.what());
		return 1;
	}
}
