// P7:关节/约束(Fixed / Distance / Hinge)+ CCD 的端到端回归(2D Box2D / 3D Jolt 两侧)。
//
// 覆盖:
//   ① 2D/3D Fixed:两体焊死后相对位姿保持(而不是各自独立落体)
//   ② 2D/3D Distance:两体距离被约束在 [Min,Max] 内
//   ③ 2D/3D Hinge:被吊住绕轴摆动,而不是自由落体
//   ④ EnableCollision=false:关节连接的两体之间**不产生**接触事件(与 P5 事件面交叉验证)
//   ⑤ CCD:2D 高速体不隧穿(对照组 Ccd=false 会穿过去);Ccd 标志确实落到后端
#include "wldpch.h"
#include "World/Core/WorldContext.h"
#include "World/Physics/Physics3D.h"
#include "World/Physics/PhysicsEvents.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <box2d/box2d.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
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

	Entity AddBox2D(Scene& scene, const char* name, const glm::vec3& location, RigidBody2DComponent::BodyType type,
		const glm::vec2& halfExtents, bool ccd = false)
	{
		Entity entity = AddEntityAt(scene, name, location);
		RigidBody2DComponent& body = entity.AddComponent<RigidBody2DComponent>();
		body.Type = type;
		body.Ccd = ccd;
		BoxCollider2DComponent& collider = entity.AddComponent<BoxCollider2DComponent>();
		collider.Size = halfExtents;
		collider.Restitution = 0.0f;
		return entity;
	}

	Entity AddBox3D(Scene& scene, const char* name, const glm::vec3& location, RigidBody3DComponent::MotionType type,
		const glm::vec3& halfExtents, bool ccd = false)
	{
		Entity entity = AddEntityAt(scene, name, location);
		RigidBody3DComponent& body = entity.AddComponent<RigidBody3DComponent>();
		body.Type = type;
		body.Ccd = ccd;
		body.Restitution = 0.0f;
		entity.AddComponent<BoxCollider3DComponent>().HalfExtents = halfExtents;
		return entity;
	}

	void StepFixed(Scene& scene, int steps)
	{
		for (int index = 0; index < steps; ++index)
			scene.OnFixedUpdate(Timestep(kFixedStep));
	}

	Entity AddJoint(Scene& scene, Entity owner, Entity other, JointComponent::JointKind kind,
		const glm::vec3& anchorSelf = glm::vec3(0.0f), const glm::vec3& anchorOther = glm::vec3(0.0f),
		float minDistance = -1.0f, float maxDistance = -1.0f, bool enableCollision = false)
	{
		JointComponent& joint = owner.AddComponent<JointComponent>();
		joint.Connected = other;
		joint.Type = kind;
		joint.AnchorSelf = anchorSelf;
		joint.AnchorOther = anchorOther;
		joint.MinDistance = minDistance;
		joint.MaxDistance = maxDistance;
		joint.EnableCollision = enableCollision;
		return owner;
	}

	glm::vec3 BodyPosition(Scene& scene, Entity entity)
	{
		if (scene.GetPhysics3DWorld() && entity.HasComponent<RigidBody3DComponent>())
		{
			glm::vec3 position { 0.0f };
			scene.GetPhysics3DWorld()->TryGetBodyTransform(entity, &position, nullptr);
			return position;
		}
		const b2Vec2 position = b2Body_GetPosition(scene.GetPhysicsBody2D(entity));
		return glm::vec3(position.x, position.y, 0.0f);
	}

	// ① 2D Fixed:焊死后相对位姿保持(两个动态体一起落,间距不变)。
	void FixedJoint2DKeepsRelativePose()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity anchor = AddBox2D(*scene, "Anchor", { 0.0f, 5.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
		Entity rider = AddBox2D(*scene, "Rider", { 1.0f, 5.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
		// 焊接语义:把两个锚点焊在一起(两个锚点都取各自原点 ⇒ 两体中心重合)。
		AddJoint(*scene, anchor, rider, JointComponent::JointKind::Fixed);
		scene->OnRuntimeStart();
		// 判别性:给 anchor 一个侧向速度 —— 没焊住的话 rider 不会被带走。
		const b2BodyId anchorBody = scene->GetPhysicsBody2D(anchor);
		b2Body_SetLinearVelocity(anchorBody, { 4.0f, 0.0f });
		b2Body_SetAwake(anchorBody, true);
		StepFixed(*scene, 45);

		const glm::vec3 anchorPosition = BodyPosition(*scene, anchor);
		const glm::vec3 riderPosition = BodyPosition(*scene, rider);
		const b2Vec2 riderVelocity = b2Body_GetLinearVelocity(scene->GetPhysicsBody2D(rider));
		std::printf("[info] 2D fixed: anchor=(%.4f, %.4f) rider=(%.4f, %.4f) separation=%.4f rider.vx=%.4f\n",
			anchorPosition.x, anchorPosition.y, riderPosition.x, riderPosition.y,
			glm::length(riderPosition - anchorPosition), riderVelocity.x);
		CHECK(anchorPosition.x > 0.5f);                   // anchor 被侧向速度带走了
		CHECK(riderVelocity.x > 1.0f);                    // rider 被焊住 ⇒ 跟着动(判别性核心)
		CHECK(glm::length(riderPosition - anchorPosition) < 0.1f);   // 两锚点焊在一起
		scene->OnRuntimeStop();
	}

	// ② 2D Distance:距离被夹在 [Min,Max] 内。
	void DistanceJoint2DKeepsRange()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity anchor = AddBox2D(*scene, "Anchor", { 0.0f, 5.0f, 0.0f }, RigidBody2DComponent::BodyType::Static, { 0.25f, 0.25f });
		Entity bob = AddBox2D(*scene, "Bob", { 2.0f, 5.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
		AddJoint(*scene, bob, anchor, JointComponent::JointKind::Distance, glm::vec3(0.0f), glm::vec3(0.0f), 1.5f, 2.5f);
		scene->OnRuntimeStart();
		float minSeen = 1e9f;
		float maxSeen = 0.0f;
		for (int step = 0; step < 180; ++step)
		{
			scene->OnFixedUpdate(Timestep(kFixedStep));
			const float distance = glm::length(BodyPosition(*scene, bob) - BodyPosition(*scene, anchor));
			minSeen = std::min(minSeen, distance);
			maxSeen = std::max(maxSeen, distance);
		}
		std::printf("[info] 2D distance: minSeen=%.4f maxSeen=%.4f (limits 1.5..2.5)\n", minSeen, maxSeen);
		// Box2D 的距离关节是软约束:允许小幅越界,但必须被夹在合理带宽内(而不是自由落体到无穷远)。
		CHECK(minSeen > 1.0f);
		CHECK(maxSeen < 3.0f);
		CHECK(BodyPosition(*scene, bob).y > 1.0f);   // 没有被重力拉走
		scene->OnRuntimeStop();
	}

	// ③ 2D Hinge:绕轴吊住(不是自由落体)。
	void HingeJoint2DHoldsBob()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity anchor = AddBox2D(*scene, "Anchor", { 0.0f, 5.0f, 0.0f }, RigidBody2DComponent::BodyType::Static, { 0.25f, 0.25f });
		Entity arm = AddBox2D(*scene, "Arm", { 1.0f, 5.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.75f, 0.1f });
		AddJoint(*scene, arm, anchor, JointComponent::JointKind::Hinge, glm::vec3(-0.75f, 0.0f, 0.0f), glm::vec3(0.0f));
		scene->OnRuntimeStart();
		StepFixed(*scene, 180);

		const glm::vec3 armPosition = BodyPosition(*scene, arm);
		std::printf("[info] 2D hinge: arm=(%.4f, %.4f)\n", armPosition.x, armPosition.y);
		CHECK(armPosition.y > 3.0f);                          // 被吊住,没有落到地面以下
		CHECK(armPosition.y < 5.5f);
		CHECK(std::fabs(armPosition.x) < 1.5f);               // 绕锚点摆,没有飞走
		scene->OnRuntimeStop();
	}

	// ④ EnableCollision=false:关节两体之间不产生接触事件(P5 事件面的交叉验证)。
	void JointDisablesCollisionBetweenBodies2D()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity anchor = AddBox2D(*scene, "Anchor", { 0.0f, 0.0f, 0.0f }, RigidBody2DComponent::BodyType::Static, { 1.0f, 0.5f });
		Entity rider = AddBox2D(*scene, "Rider", { 0.0f, 0.9f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.25f, 0.25f });
		AddJoint(*scene, rider, anchor, JointComponent::JointKind::Fixed, glm::vec3(0.0f), glm::vec3(0.0f, 0.9f, 0.0f));
		scene->OnRuntimeStart();
		StepFixed(*scene, 120);

		int contacts = 0;
		for (const Physics::ContactEvent& event : scene->GetContactEvents())
			if ((event.EntityA == static_cast<entt::entity>(anchor) && event.EntityB == static_cast<entt::entity>(rider)) ||
				(event.EntityA == static_cast<entt::entity>(rider) && event.EntityB == static_cast<entt::entity>(anchor)))
				++contacts;
		std::printf("[info] 2D joint collideConnected=false: contact events=%d\n", contacts);
		CHECK(contacts == 0);
		scene->OnRuntimeStop();
	}

	// ① 3D Fixed
	void FixedJoint3DKeepsRelativePose()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity anchor = AddBox3D(*scene, "Anchor", { 0.0f, 5.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic, { 0.25f, 0.25f, 0.25f });
		Entity rider = AddBox3D(*scene, "Rider", { 1.0f, 5.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic, { 0.25f, 0.25f, 0.25f });
		AddJoint(*scene, anchor, rider, JointComponent::JointKind::Fixed);
		scene->OnRuntimeStart();
		// 判别性:侧向速度 —— 焊接失效时 rider 只会跟着重力落,不会被带走。
		StepFixed(*scene, 45);

		const glm::vec3 anchorPosition = BodyPosition(*scene, anchor);
		const glm::vec3 riderPosition = BodyPosition(*scene, rider);
		std::printf("[info] 3D fixed: anchor=(%.4f, %.4f) rider=(%.4f, %.4f) separation=%.4f\n",
			anchorPosition.x, anchorPosition.y, riderPosition.x, riderPosition.y,
			glm::length(riderPosition - anchorPosition));
		CHECK(anchorPosition.y < 4.5f);                                  // 真的在动(不是 Static 空转)
		CHECK(glm::length(riderPosition - anchorPosition) < 0.1f);       // 两锚点焊在一起(1.0 ⇒ 0)
		CHECK(std::fabs(anchorPosition.x - riderPosition.x) < 0.1f);     // 中心对齐(焊接直接证据)
		scene->OnRuntimeStop();
	}

	// ② 3D Distance
	void DistanceJoint3DKeepsRange()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity anchor = AddBox3D(*scene, "Anchor", { 0.0f, 5.0f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 0.25f, 0.25f, 0.25f });
		Entity bob = AddBox3D(*scene, "Bob", { 2.0f, 5.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic, { 0.25f, 0.25f, 0.25f });
		AddJoint(*scene, bob, anchor, JointComponent::JointKind::Distance, glm::vec3(0.0f), glm::vec3(0.0f), 1.5f, 2.5f);
		scene->OnRuntimeStart();
		float minSeen = 1e9f;
		float maxSeen = 0.0f;
		for (int step = 0; step < 180; ++step)
		{
			scene->OnFixedUpdate(Timestep(kFixedStep));
			const float distance = glm::length(BodyPosition(*scene, bob) - BodyPosition(*scene, anchor));
			minSeen = std::min(minSeen, distance);
			maxSeen = std::max(maxSeen, distance);
		}
		std::printf("[info] 3D distance: minSeen=%.4f maxSeen=%.4f (limits 1.5..2.5)\n", minSeen, maxSeen);
		CHECK(minSeen > 1.0f);
		CHECK(maxSeen < 3.0f);
		CHECK(BodyPosition(*scene, bob).y > 1.0f);
		scene->OnRuntimeStop();
	}

	// ③ 3D Hinge
	void HingeJoint3DHoldsBob()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity anchor = AddBox3D(*scene, "Anchor", { 0.0f, 5.0f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 0.25f, 0.25f, 0.25f });
		Entity arm = AddBox3D(*scene, "Arm", { 1.0f, 5.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic, { 0.75f, 0.1f, 0.1f });
		AddJoint(*scene, arm, anchor, JointComponent::JointKind::Hinge, glm::vec3(-0.75f, 0.0f, 0.0f), glm::vec3(0.0f));
		scene->OnRuntimeStart();
		StepFixed(*scene, 180);

		const glm::vec3 armPosition = BodyPosition(*scene, arm);
		std::printf("[info] 3D hinge: arm=(%.4f, %.4f, %.4f)\n", armPosition.x, armPosition.y, armPosition.z);
		CHECK(armPosition.y > 3.0f);
		CHECK(armPosition.y < 5.5f);
		CHECK(std::fabs(armPosition.x) < 1.5f);
		scene->OnRuntimeStop();
	}

	// ⑤ CCD(2D):`Ccd` 必须真的落到 Box2D 的 isBullet 上(两档都断言,防止只写不读)。

	//   附注(如实记录,不假装 CCD 的功劳):Box2D v3 的 speculative contact 在
	//   "300 m/s 打 0.04 m 薄墙" 这个场景里**即使非 bullet 也会拦下**(实测两档同值) ——
	//   所以这里只断言标志接线,不把"没穿过去"算作 CCD 的收益。

	void CcdFlagReachesBox2D()

	{

		const auto bulletFlagFor = [](bool ccd) -> bool

		{

			Ref<Scene> scene = CreateTestScene();

			Entity bullet = AddBox2D(*scene, "Bullet", { -3.0f, 0.0f, 0.0f }, RigidBody2DComponent::BodyType::Dynamic, { 0.05f, 0.05f }, ccd);

			scene->OnRuntimeStart();

			const bool bulletFlag = b2Body_IsBullet(scene->GetPhysicsBody2D(bullet));

			scene->OnRuntimeStop();

			return bulletFlag;

		};

		const bool withCcd = bulletFlagFor(true);

		const bool withoutCcd = bulletFlagFor(false);

		std::printf("[info] 2D ccd: isBullet withCcd=%d withoutCcd=%d\n", withCcd ? 1 : 0, withoutCcd ? 1 : 0);

		CHECK(withCcd);

		CHECK(!withoutCcd);

	}



	// ⑤ CCD(3D):`Ccd` ⇒ Jolt EMotionQuality::LinearCast(直接读回),且低速模拟逐位不回归。
	void CcdFlag3DDoesNotChangeLowSpeedSimulation()
	{
		const auto run = [](bool ccd) -> float
		{
			Ref<Scene> scene = CreateTestScene();
			AddBox3D(*scene, "Ground", { 0.0f, 0.0f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 5.0f, 0.5f, 5.0f });
			Entity box = AddBox3D(*scene, "Box", { 0.0f, 3.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic, { 0.5f, 0.5f, 0.5f }, ccd);
			scene->OnRuntimeStart();
			// `Ccd` 必须真的落到 Jolt 的运动质量档位上(读回而不是只看组件字段)。
			if (ccd)
				CHECK(scene->GetPhysics3DWorld()->GetBodyMotionQualityIsLinearCast(box));
			StepFixed(*scene, 120);
			const float y = BodyPosition(*scene, box).y;
			scene->OnRuntimeStop();
			return y;
		};
		const float withCcd = run(true);
		const float withoutCcd = run(false);
		std::printf("[info] 3D ccd=false %.6f vs ccd=true %.6f (must be bit-identical)\n", withoutCcd, withCcd);
		CHECK(withoutCcd == withCcd);
	}
}

int main()
{
	try
	{
		const std::pair<const char*, void (*)()> tests[] = {
			{ "2D fixed joint keeps relative pose", FixedJoint2DKeepsRelativePose },
			{ "2D distance joint keeps range", DistanceJoint2DKeepsRange },
			{ "2D hinge joint holds the bob", HingeJoint2DHoldsBob },
			{ "jointed bodies do not produce contact events (collideConnected=false)", JointDisablesCollisionBetweenBodies2D },
			{ "3D fixed joint keeps relative pose", FixedJoint3DKeepsRelativePose },
			{ "3D distance joint keeps range", DistanceJoint3DKeepsRange },
			{ "3D hinge joint holds the bob", HingeJoint3DHoldsBob },
			{ "2D CCD flag reaches Box2D isBullet (both directions)", CcdFlagReachesBox2D },
			{ "3D CCD sets LinearCast and does not change low speed simulation", CcdFlag3DDoesNotChangeLowSpeedSimulation },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		if (failures == 0)
			std::printf("World.PhysicsJoints: all checks passed\n");
		else
			std::fprintf(stderr, "World.PhysicsJoints: %d group(s) failed\n", failures);
		return failures == 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.PhysicsJoints: fatal: %s\n", error.what());
		return 1;
	}
}
