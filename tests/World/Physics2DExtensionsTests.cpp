#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Physics/Physics2DUtils.h"
#include <box2d/box2d.h>
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"

#include <iostream>
#include <cassert>
#include <cmath>

using namespace World;

static void TestPolygonDecomposition()
{
	// 1. 凸多边形检测
	{
		std::vector<glm::vec2> quad = {
			{ -1.0f, -1.0f },
			{  1.0f, -1.0f },
			{  1.0f,  1.0f },
			{ -1.0f,  1.0f }
		};
		assert(Physics2D::IsPolygonConvex(quad));
		auto decomp = Physics2D::DecomposeOrTriangulatePolygon(quad);
		assert(decomp.size() == 1);
		assert(decomp[0].size() == 4);
	}

	// 2. 凹多边形检测与耳切法分解 (L 形多边形 6 个顶点)
	{
		std::vector<glm::vec2> lShape = {
			{ 0.0f, 0.0f },
			{ 2.0f, 0.0f },
			{ 2.0f, 1.0f },
			{ 1.0f, 1.0f },
			{ 1.0f, 2.0f },
			{ 0.0f, 2.0f }
		};
		assert(!Physics2D::IsPolygonConvex(lShape));
		auto decomp = Physics2D::DecomposeOrTriangulatePolygon(lShape);
		assert(decomp.size() == 4); // N - 2 = 6 - 2 = 4 个三角形
		for (const auto& tri : decomp)
		{
			assert(tri.size() == 3);
			assert(Physics2D::ComputePolygonSignedArea(tri) > 0.0f); // 逆时针
		}
	}

	std::cout << "[PASS] TestPolygonDecomposition\n";
}

static void TestPolygonAndChainPhysicsSimulation()
{
	WorldContext context;
	Scene scene(context);

	// 1. 创建地表链条碰撞体 (ChainCollider2DComponent)
	Entity groundEntity = scene.CreateEntityShell("GroundChain");
	auto& groundTransform = groundEntity.AddComponent<TransformComponent>();
	groundTransform.Location = { 0.0f, 0.0f, 0.0f };
	auto& groundChain = groundEntity.AddComponent<ChainCollider2DComponent>();
	// 凹地水槽轮廓: 左高 (-5, 2) -> 下坡 (-2, 0) -> 平底 (2, 0) -> 右高 (5, 2)
	groundChain.SetPoints({
		{ -5.0f, 2.0f },
		{ -2.0f, 0.0f },
		{  2.0f, 0.0f },
		{  5.0f, 2.0f }
	});

	// 2. 创建动态多边形刚体 (PolygonCollider2DComponent)
	Entity boxEntity = scene.CreateEntityShell("DynamicPoly");
	auto& boxTransform = boxEntity.AddComponent<TransformComponent>();
	boxTransform.Location = { 0.0f, 4.0f, 0.0f };
	auto& boxRb = boxEntity.AddComponent<RigidBody2DComponent>();
	boxRb.Type = RigidBody2DComponent::BodyType::Dynamic;
	auto& boxPoly = boxEntity.AddComponent<PolygonCollider2DComponent>();
	boxPoly.SetPoints({
		{ -0.5f, -0.5f },
		{  0.5f, -0.5f },
		{  0.0f,  0.5f }
	});

	// 启动物理世界并模拟若干帧
	scene.OnRuntimeStart();
	assert(scene.IsPhysics2DRunning());

	// 下落前 Y 坐标为 4.0
	assert(boxTransform.Location.y == 4.0f);

	for (int i = 0; i < 60; ++i)
	{
		scene.OnFixedUpdate(Timestep(0.016f));
	}

	// 落地后停留在地表链条凹地内 (落点 Y 稳定在 -0.6 ~ 0.5 之间，绝不掉出虚空)
	assert(boxTransform.Location.y > -1.0f && boxTransform.Location.y < 2.0f);

	scene.OnRuntimeStop();
	assert(!scene.IsPhysics2DRunning());

	std::cout << "[PASS] TestPolygonAndChainPhysicsSimulation\n";
}

static void TestPlatformEffectorOneWay()
{
	WorldContext context;

	// 1. 测试从下方向上跳跃穿透 (RelativeVelocity.y > 0)
	{
		const float surfaceArc = 180.0f;
		const float rotOffset = 0.0f;
		const float rotZ = 0.0f;
		const glm::vec2 normFromPlatformToOther { 0.0f, 1.0f }; // 位于上方
		const glm::vec2 velMovingUp { 0.0f, 5.0f };            // 向上跳动
		bool collide = Physics2D::EvaluatePlatformEffector(surfaceArc, rotOffset, rotZ, normFromPlatformToOther, velMovingUp);
		assert(!collide); // 向上跳动时穿透通过
	}

	// 2. 测试从上方下落接触 (RelativeVelocity.y <= 0)
	{
		const float surfaceArc = 180.0f;
		const float rotOffset = 0.0f;
		const float rotZ = 0.0f;
		const glm::vec2 normFromPlatformToOther { 0.0f, 1.0f }; // 位于上方
		const glm::vec2 velFallingDown { 0.0f, -2.0f };         // 向下掉落
		bool collide = Physics2D::EvaluatePlatformEffector(surfaceArc, rotOffset, rotZ, normFromPlatformToOther, velFallingDown);
		assert(collide); // 坚固碰撞阻挡
	}

	// 3. 测试从正下方接触 (法线夹角超过半球 180° 范围)
	{
		const float surfaceArc = 180.0f;
		const float rotOffset = 0.0f;
		const float rotZ = 0.0f;
		const glm::vec2 normFromBottom { 0.0f, -1.0f }; // 位于平台下方
		const glm::vec2 velFallingDown { 0.0f, -2.0f };
		bool collide = Physics2D::EvaluatePlatformEffector(surfaceArc, rotOffset, rotZ, normFromBottom, velFallingDown);
		assert(!collide); // 下方不产生碰撞
	}

	// 4. 实机物理世界测试: 角色从平台下方跃起穿透
	{
		Scene scene(context);

		// 平台在 Y = 2.0
		Entity platform = scene.CreateEntityShell("Platform");
		auto& platT = platform.AddComponent<TransformComponent>();
		platT.Location = { 0.0f, 2.0f, 0.0f };
		auto& platBox = platform.AddComponent<BoxCollider2DComponent>();
		platBox.Size = { 3.0f, 0.2f };
		auto& effector = platform.AddComponent<PlatformEffector2DComponent>();
		effector.OneWay = true;
		effector.SurfaceArc = 180.0f;

		// 角色从 Y = 0.0 向上发射
		Entity jumper = scene.CreateEntityShell("Jumper");
		auto& jumperT = jumper.AddComponent<TransformComponent>();
		jumperT.Location = { 0.0f, 0.0f, 0.0f };
		auto& jumperRb = jumper.AddComponent<RigidBody2DComponent>();
		jumperRb.Type = RigidBody2DComponent::BodyType::Dynamic;
		auto& jumperBox = jumper.AddComponent<BoxCollider2DComponent>();
		jumperBox.Size = { 0.3f, 0.3f };

		scene.OnRuntimeStart();
		b2BodyId jumperBody = scene.GetPhysicsBody2D(jumper);
		assert(b2Body_IsValid(jumperBody));
		b2Body_SetLinearVelocity(jumperBody, { 0.0f, 15.0f });

		// 模拟 15 步 (约 0.24s)，角色向上穿过 Y = 2.0 平台到达 Y > 2.0
		for (int i = 0; i < 15; ++i)
		{
			scene.OnFixedUpdate(Timestep(0.016f));
		}
		assert(jumperT.Location.y > 2.0f); // 成功穿透平台到达上方

		scene.OnRuntimeStop();
	}

	std::cout << "[PASS] TestPlatformEffectorOneWay\n";
}

static void TestJoints2DComponentPODAndDefaults()
{
	// 1. 验证 2D 物理关节组件尺寸契约 (48B / 48B / 56B / 64B)
	static_assert(sizeof(DistanceJoint2DComponent) == 40, "DistanceJoint2DComponent size mismatch");
	static_assert(sizeof(RevoluteJoint2DComponent) == 40, "RevoluteJoint2DComponent size mismatch");
	static_assert(sizeof(PrismaticJoint2DComponent) == 48, "PrismaticJoint2DComponent size mismatch");
	static_assert(sizeof(WheelJoint2DComponent) == 56, "WheelJoint2DComponent size mismatch");

	// 2. 验证平凡可复制 (POD / Component layout contract)
	static_assert(std::is_trivially_copyable_v<DistanceJoint2DComponent>, "DistanceJoint2DComponent must be trivially copyable");
	static_assert(std::is_trivially_copyable_v<RevoluteJoint2DComponent>, "RevoluteJoint2DComponent must be trivially copyable");
	static_assert(std::is_trivially_copyable_v<PrismaticJoint2DComponent>, "PrismaticJoint2DComponent must be trivially copyable");
	static_assert(std::is_trivially_copyable_v<WheelJoint2DComponent>, "WheelJoint2DComponent must be trivially copyable");

	// 3. 验证默认字段初值
	{
		DistanceJoint2DComponent dist;
		assert(dist.ConnectedEntity == entt::null);
		assert(dist.AnchorSelf.x == 0.0f && dist.AnchorSelf.y == 0.0f);
		assert(dist.AnchorOther.x == 0.0f && dist.AnchorOther.y == 0.0f);
		assert(dist.MinDistance == -1.0f);
		assert(dist.MaxDistance == -1.0f);
		assert(dist.Hertz == 0.0f);
		assert(dist.DampingRatio == 0.7f);
		assert(!dist.EnableLimit);
		assert(!dist.EnableSpring);
		assert(!dist.EnableCollision);
	}

	{
		RevoluteJoint2DComponent rev;
		assert(rev.ConnectedEntity == entt::null);
		assert(rev.AnchorSelf.x == 0.0f && rev.AnchorSelf.y == 0.0f);
		assert(rev.AnchorOther.x == 0.0f && rev.AnchorOther.y == 0.0f);
		assert(rev.LowerAngle == 0.0f);
		assert(rev.UpperAngle == 0.0f);
		assert(rev.MotorSpeed == 0.0f);
		assert(rev.MaxMotorTorque == 10.0f);
		assert(!rev.EnableLimit);
		assert(!rev.EnableMotor);
		assert(!rev.EnableCollision);
	}

	{
		PrismaticJoint2DComponent prism;
		assert(prism.ConnectedEntity == entt::null);
		assert(prism.AnchorSelf.x == 0.0f && prism.AnchorSelf.y == 0.0f);
		assert(prism.AnchorOther.x == 0.0f && prism.AnchorOther.y == 0.0f);
		assert(prism.Axis.x == 1.0f && prism.Axis.y == 0.0f);
		assert(prism.LowerTranslation == -1.0f);
		assert(prism.UpperTranslation == 1.0f);
		assert(prism.MotorSpeed == 0.0f);
		assert(prism.MaxMotorForce == 50.0f);
		assert(!prism.EnableLimit);
		assert(!prism.EnableMotor);
		assert(!prism.EnableCollision);
	}

	{
		WheelJoint2DComponent wheel;
		assert(wheel.ConnectedEntity == entt::null);
		assert(wheel.AnchorSelf.x == 0.0f && wheel.AnchorSelf.y == 0.0f);
		assert(wheel.AnchorOther.x == 0.0f && wheel.AnchorOther.y == 0.0f);
		assert(wheel.SuspensionAxis.x == 0.0f && wheel.SuspensionAxis.y == 1.0f);
		assert(wheel.Hertz == 4.0f);
		assert(wheel.DampingRatio == 0.7f);
		assert(wheel.LowerTranslation == -0.2f);
		assert(wheel.UpperTranslation == 0.2f);
		assert(wheel.MotorSpeed == 0.0f);
		assert(wheel.MaxMotorTorque == 20.0f);
		assert(wheel.EnableLimit);
		assert(!wheel.EnableMotor);
		assert(!wheel.EnableCollision);
	}

	std::cout << "[PASS] TestJoints2DComponentPODAndDefaults\n";
}

static void TestRevoluteJoint2DPendulumSimulation()
{
	WorldContext context;
	Scene scene(context);

	// 1. 创建静态锚点实体 (Anchor Pin)
	Entity anchor = scene.CreateEntityShell("AnchorPin");
	auto& anchorT = anchor.AddComponent<TransformComponent>();
	anchorT.Location = { 0.0f, 5.0f, 0.0f };
	auto& anchorRb = anchor.AddComponent<RigidBody2DComponent>();
	anchorRb.Type = RigidBody2DComponent::BodyType::Static;
	auto& anchorBox = anchor.AddComponent<BoxCollider2DComponent>();
	anchorBox.Size = { 0.5f, 0.5f };

	// 2. 创建动态摆球实体 (Pendulum Bob)，横向偏离放置在 (2.0, 5.0, 0.0) 以在重力作用下发生摆动
	Entity bob = scene.CreateEntityShell("PendulumBob");
	auto& bobT = bob.AddComponent<TransformComponent>();
	bobT.Location = { 2.0f, 5.0f, 0.0f };
	auto& bobRb = bob.AddComponent<RigidBody2DComponent>();
	bobRb.Type = RigidBody2DComponent::BodyType::Dynamic;
	auto& bobBox = bob.AddComponent<BoxCollider2DComponent>();
	bobBox.Size = { 0.5f, 0.5f };
	bobBox.Density = 1.0f;

	// 挂载 RevoluteJoint2DComponent
	// 锚点在世界空间中位于 (0.0, 5.0)
	// 对于 anchor (0, 5): 局部锚点为 (0, 0)
	// 对于 bob (2, 5): 局部锚点为 (-2, 0)
	auto& revJoint = bob.AddComponent<RevoluteJoint2DComponent>();
	revJoint.ConnectedEntity = static_cast<entt::entity>(anchor);
	revJoint.AnchorSelf = { -2.0f, 0.0f };
	revJoint.AnchorOther = { 0.0f, 0.0f };
	revJoint.EnableCollision = false;

	// 启动物理世界
	scene.OnRuntimeStart();
	assert(scene.IsPhysics2DRunning());

	b2JointId jointId = scene.GetPhysicsJoint2D(bob);
	assert(b2Joint_IsValid(jointId));

	const float initialDist = glm::distance(glm::vec2(bobT.Location), glm::vec2(anchorT.Location));
	assert(std::abs(initialDist - 2.0f) < 0.01f);

	// 步进物理模拟若干帧 (使用 scene.OnUpdatePhysics2D(0.016f))
	const float initialBobX = bobT.Location.x;
	for (int i = 0; i < 60; ++i)
	{
		scene.OnFixedUpdate(Timestep(0.016f));

		// 钟摆摆动过程中，与锚点在世界空间中的距离始终稳定维持在 2.0m 附近
		float curDist = glm::distance(glm::vec2(bobT.Location), glm::vec2(anchorT.Location));
		assert(std::abs(curDist - 2.0f) < 0.08f);
	}

	// 60 帧后，在重力作用下拉动下坠，Y 坐标发生明显降低 (从 5.0 下落)，X 坐标发生移动
	assert(bobT.Location.y < 4.95f);
	assert(bobT.Location.x != initialBobX);

	// 验证 scene.OnRuntimeStop() 时 2D 关节安全销毁无泄漏
	scene.OnRuntimeStop();
	assert(!scene.IsPhysics2DRunning());
	assert(!b2Joint_IsValid(scene.GetPhysicsJoint2D(bob)));

	std::cout << "[PASS] TestRevoluteJoint2DPendulumSimulation\n";
}

static void TestDistanceJoint2DSimulation()
{
	WorldContext context;

	// 1. 刚性杆距离约束测试 (Rigid Rod Distance Constraint)
	{
		Scene scene(context);

		Entity anchor = scene.CreateEntityShell("DistAnchor");
		auto& anchorT = anchor.AddComponent<TransformComponent>();
		anchorT.Location = { 0.0f, 0.0f, 0.0f };
		auto& anchorRb = anchor.AddComponent<RigidBody2DComponent>();
		anchorRb.Type = RigidBody2DComponent::BodyType::Static;
		auto& anchorBox = anchor.AddComponent<BoxCollider2DComponent>();
		anchorBox.Size = { 0.5f, 0.5f };

		Entity weight = scene.CreateEntityShell("DistWeight");
		auto& weightT = weight.AddComponent<TransformComponent>();
		weightT.Location = { 0.0f, -2.0f, 0.0f };
		auto& weightRb = weight.AddComponent<RigidBody2DComponent>();
		weightRb.Type = RigidBody2DComponent::BodyType::Dynamic;
		auto& weightBox = weight.AddComponent<BoxCollider2DComponent>();
		weightBox.Size = { 0.5f, 0.5f };
		weightBox.Density = 1.0f;

		auto& distJoint = weight.AddComponent<DistanceJoint2DComponent>();
		distJoint.ConnectedEntity = static_cast<entt::entity>(anchor);
		distJoint.AnchorSelf = { 0.0f, 0.0f };
		distJoint.AnchorOther = { 0.0f, 0.0f };
		// 默认刚性杆 (Hertz = 0, EnableSpring = false)

		scene.OnRuntimeStart();
		assert(scene.IsPhysics2DRunning());

		b2JointId jointId = scene.GetPhysicsJoint2D(weight);
		assert(b2Joint_IsValid(jointId));

		// 步进 60 步，在重力作用下，刚性杆将 weight 保持在距离 anchor 约 2.0m 处 (不会自由落体掉穿)
		for (int i = 0; i < 60; ++i)
		{
			scene.OnFixedUpdate(Timestep(0.016f));
		}

		const float finalDist = glm::distance(glm::vec2(weightT.Location), glm::vec2(anchorT.Location));
		assert(std::abs(finalDist - 2.0f) < 0.06f);
		assert(weightT.Location.y > -2.15f && weightT.Location.y < -1.85f);

		scene.OnRuntimeStop();
		assert(!scene.IsPhysics2DRunning());
		assert(!b2Joint_IsValid(scene.GetPhysicsJoint2D(weight)));
	}

	// 2. 弹簧阻尼弹性约束测试 (Spring-Damper Distance Constraint)
	{
		Scene scene(context);

		Entity anchor = scene.CreateEntityShell("SpringAnchor");
		auto& anchorT = anchor.AddComponent<TransformComponent>();
		anchorT.Location = { 0.0f, 0.0f, 0.0f };
		auto& anchorRb = anchor.AddComponent<RigidBody2DComponent>();
		anchorRb.Type = RigidBody2DComponent::BodyType::Static;
		auto& anchorBox = anchor.AddComponent<BoxCollider2DComponent>();
		anchorBox.Size = { 0.5f, 0.5f };

		Entity bob = scene.CreateEntityShell("SpringBob");
		auto& bobT = bob.AddComponent<TransformComponent>();
		bobT.Location = { 0.0f, -1.0f, 0.0f };
		auto& bobRb = bob.AddComponent<RigidBody2DComponent>();
		bobRb.Type = RigidBody2DComponent::BodyType::Dynamic;
		auto& bobBox = bob.AddComponent<BoxCollider2DComponent>();
		bobBox.Size = { 0.5f, 0.5f };
		bobBox.Density = 1.0f;

		auto& distJoint = bob.AddComponent<DistanceJoint2DComponent>();
		distJoint.ConnectedEntity = static_cast<entt::entity>(anchor);
		distJoint.AnchorSelf = { 0.0f, 0.0f };
		distJoint.AnchorOther = { 0.0f, 0.0f };
		distJoint.EnableSpring = true;
		distJoint.Hertz = 3.0f;
		distJoint.DampingRatio = 0.5f;

		scene.OnRuntimeStart();
		assert(scene.IsPhysics2DRunning());
		b2JointId jointId = scene.GetPhysicsJoint2D(bob);
		assert(b2Joint_IsValid(jointId));

		// 步进模拟若干步，弹簧在重力拉扯下伸长但被弹簧恢复力约束
		for (int i = 0; i < 40; ++i)
		{
			scene.OnFixedUpdate(Timestep(0.016f));
		}

		const float springDist = glm::distance(glm::vec2(bobT.Location), glm::vec2(anchorT.Location));
		assert(springDist > 1.0f && springDist < 4.0f);

		scene.OnRuntimeStop();
		assert(!scene.IsPhysics2DRunning());
	}

	std::cout << "[PASS] TestDistanceJoint2DSimulation\n";
}

static void TestPrismaticAndWheelJointsLifecycle()
{
	WorldContext context;
	Scene scene(context);

	// Prismatic joint
	Entity track = scene.CreateEntityShell("Track");
	auto& trackT = track.AddComponent<TransformComponent>();
	trackT.Location = { 0.0f, 0.0f, 0.0f };
	auto& trackRb = track.AddComponent<RigidBody2DComponent>();
	trackRb.Type = RigidBody2DComponent::BodyType::Static;
	track.AddComponent<BoxCollider2DComponent>().Size = { 1.0f, 0.2f };

	Entity slider = scene.CreateEntityShell("Slider");
	auto& sliderT = slider.AddComponent<TransformComponent>();
	sliderT.Location = { 0.0f, 0.0f, 0.0f };
	auto& sliderRb = slider.AddComponent<RigidBody2DComponent>();
	sliderRb.Type = RigidBody2DComponent::BodyType::Dynamic;
	slider.AddComponent<BoxCollider2DComponent>().Size = { 0.4f, 0.4f };

	auto& prism = slider.AddComponent<PrismaticJoint2DComponent>();
	prism.ConnectedEntity = static_cast<entt::entity>(track);
	prism.Axis = { 1.0f, 0.0f };
	prism.EnableLimit = true;
	prism.LowerTranslation = -2.0f;
	prism.UpperTranslation = 2.0f;

	// Wheel joint
	Entity carBody = scene.CreateEntityShell("CarBody");
	auto& carT = carBody.AddComponent<TransformComponent>();
	carT.Location = { 10.0f, 0.0f, 0.0f };
	auto& carRb = carBody.AddComponent<RigidBody2DComponent>();
	carRb.Type = RigidBody2DComponent::BodyType::Dynamic;
	carBody.AddComponent<BoxCollider2DComponent>().Size = { 2.0f, 0.5f };

	Entity wheelEnt = scene.CreateEntityShell("Wheel");
	auto& wheelT = wheelEnt.AddComponent<TransformComponent>();
	wheelT.Location = { 9.0f, -0.5f, 0.0f };
	auto& wheelRb = wheelEnt.AddComponent<RigidBody2DComponent>();
	wheelRb.Type = RigidBody2DComponent::BodyType::Dynamic;
	wheelEnt.AddComponent<CircleCollider2DComponent>().Radius = 0.3f;

	auto& wheel = wheelEnt.AddComponent<WheelJoint2DComponent>();
	wheel.ConnectedEntity = static_cast<entt::entity>(carBody);
	wheel.SuspensionAxis = { 0.0f, 1.0f };
	wheel.Hertz = 5.0f;
	wheel.DampingRatio = 0.7f;

	scene.OnRuntimeStart();
	assert(scene.IsPhysics2DRunning());

	b2JointId prismId = scene.GetPhysicsJoint2D(slider);
	b2JointId wheelId = scene.GetPhysicsJoint2D(wheelEnt);
	assert(b2Joint_IsValid(prismId));
	assert(b2Joint_IsValid(wheelId));

	for (int i = 0; i < 20; ++i)
	{
		scene.OnFixedUpdate(Timestep(0.016f));
	}

	scene.OnRuntimeStop();
	assert(!scene.IsPhysics2DRunning());
	assert(!b2Joint_IsValid(scene.GetPhysicsJoint2D(slider)));
	assert(!b2Joint_IsValid(scene.GetPhysicsJoint2D(wheelEnt)));

	std::cout << "[PASS] TestPrismaticAndWheelJointsLifecycle\n";
}


static void TestEffectors2DComponentPODAndDefaults()
{
	static_assert(sizeof(AreaEffector2DComponent) == 28, "AreaEffector2DComponent must be exactly 28 bytes");
	static_assert(std::is_trivially_copyable_v<AreaEffector2DComponent>, "AreaEffector2DComponent must be trivially copyable");

	static_assert(sizeof(BuoyancyEffector2DComponent) == 32, "BuoyancyEffector2DComponent must be exactly 32 bytes");
	static_assert(std::is_trivially_copyable_v<BuoyancyEffector2DComponent>, "BuoyancyEffector2DComponent must be trivially copyable");

	static_assert(sizeof(SurfaceEffector2DComponent) == 16, "SurfaceEffector2DComponent must be exactly 16 bytes");
	static_assert(std::is_trivially_copyable_v<SurfaceEffector2DComponent>, "SurfaceEffector2DComponent must be trivially copyable");

	// 1. AreaEffector2DComponent defaults
	{
		AreaEffector2DComponent area;
		assert(area.ForceDirection.x == 0.0f && area.ForceDirection.y == 1.0f);
		assert(area.ForceMagnitude == 10.0f);
		assert(area.LinearDrag == 0.5f);
		assert(area.AngularDrag == 0.5f);
		assert(area.ForceMode == 0);
		assert(!area.UseGlobalCoordinates);
		assert(!area.AffectsInertia);
		assert(area.Enabled);
	}

	// 2. BuoyancyEffector2DComponent defaults
	{
		BuoyancyEffector2DComponent buoy;
		assert(buoy.Density == 2.0f);
		assert(buoy.SurfaceLevel == 0.0f);
		assert(buoy.LinearDrag == 2.5f);
		assert(buoy.AngularDrag == 1.5f);
		assert(buoy.FlowVelocity.x == 0.0f && buoy.FlowVelocity.y == 0.0f);
		assert(buoy.Enabled);
	}

	// 3. SurfaceEffector2DComponent defaults
	{
		SurfaceEffector2DComponent surf;
		assert(surf.Speed == 3.0f);
		assert(surf.ForceScale == 1.0f);
		assert(surf.Enabled);
	}

	std::cout << "[PASS] TestEffectors2DComponentPODAndDefaults\n";
}

static void TestAreaEffector2DWindSimulation()
{
	WorldContext context;
	Scene scene(context);

	// 1. 创建上升气流传感器区域 (AreaEffector2DComponent)
	Entity windArea = scene.CreateEntityShell("WindArea");
	auto& windT = windArea.AddComponent<TransformComponent>();
	windT.Location = { 0.0f, 0.0f, 0.0f };
	auto& windRb = windArea.AddComponent<RigidBody2DComponent>();
	windRb.Type = RigidBody2DComponent::BodyType::Static;
	auto& windBox = windArea.AddComponent<BoxCollider2DComponent>();
	windBox.Size = { 5.0f, 5.0f };
	windBox.IsSensor = true;
	auto& area = windArea.AddComponent<AreaEffector2DComponent>();
	area.ForceDirection = { 0.0f, 1.0f };
	area.ForceMagnitude = 30.0f;
	area.ForceMode = 0; // Directional
	area.LinearDrag = 0.5f;

	// 2. 创建置于气流中的动态刚体 (初始 Y = 0.0)
	Entity floater = scene.CreateEntityShell("Floater");
	auto& floaterT = floater.AddComponent<TransformComponent>();
	floaterT.Location = { 0.0f, 0.0f, 0.0f };
	auto& floaterRb = floater.AddComponent<RigidBody2DComponent>();
	floaterRb.Type = RigidBody2DComponent::BodyType::Dynamic;
	auto& floaterBox = floater.AddComponent<BoxCollider2DComponent>();
	floaterBox.Size = { 0.4f, 0.4f };
	floaterBox.Density = 1.0f;

	scene.OnRuntimeStart();
	assert(scene.IsPhysics2DRunning());

	// 模拟 40 步 (约 0.64s)，物体在风力作用下克服重力向上升起 (Location.y 显著增加)
	for (int i = 0; i < 40; ++i)
	{
		scene.OnFixedUpdate(Timestep(0.016f));
	}

	assert(floaterT.Location.y > 1.0f);

	scene.OnRuntimeStop();
	assert(!scene.IsPhysics2DRunning());

	std::cout << "[PASS] TestAreaEffector2DWindSimulation\n";
}

static void TestBuoyancyEffector2DFloatingSimulation()
{
	WorldContext context;
	Scene scene(context);

	// 1. 创建水体传感器区域 (BuoyancyEffector2DComponent)
	// 中心位于 Y = -2.0, 尺寸 10x2 (Y 范围 -4.0 ~ 0.0), 水面高度 local +2.0 => world Y = 0.0
	Entity water = scene.CreateEntityShell("WaterPool");
	auto& waterT = water.AddComponent<TransformComponent>();
	waterT.Location = { 0.0f, -2.0f, 0.0f };
	auto& waterRb = water.AddComponent<RigidBody2DComponent>();
	waterRb.Type = RigidBody2DComponent::BodyType::Static;
	auto& waterBox = water.AddComponent<BoxCollider2DComponent>();
	waterBox.Size = { 10.0f, 2.0f };
	waterBox.IsSensor = true;
	auto& buoy = water.AddComponent<BuoyancyEffector2DComponent>();
	buoy.Density = 2.5f;
	buoy.SurfaceLevel = 2.0f;
	buoy.LinearDrag = 3.0f;

	// 2. 创建从水面上方落下的动态刚体 (初始 Y = 1.0)
	Entity boat = scene.CreateEntityShell("Boat");
	auto& boatT = boat.AddComponent<TransformComponent>();
	boatT.Location = { 0.0f, 1.0f, 0.0f };
	auto& boatRb = boat.AddComponent<RigidBody2DComponent>();
	boatRb.Type = RigidBody2DComponent::BodyType::Dynamic;
	auto& boatBox = boat.AddComponent<BoxCollider2DComponent>();
	boatBox.Size = { 0.4f, 0.4f };
	boatBox.Density = 1.0f;

	scene.OnRuntimeStart();
	assert(scene.IsPhysics2DRunning());

	// 模拟 60 步 (约 0.96s)，下落刚体浸入水体后受浮力阻止继续无限下坠并浮起
	for (int i = 0; i < 60; ++i)
	{
		scene.OnFixedUpdate(Timestep(0.016f));
	}

	// 浸入水体后受浮力支撑, 维持在水面附近 (Y > -1.5 且 < 0.5), 不会深坠虚空
	assert(boatT.Location.y > -1.5f && boatT.Location.y < 0.5f);

	scene.OnRuntimeStop();
	assert(!scene.IsPhysics2DRunning());

	std::cout << "[PASS] TestBuoyancyEffector2DFloatingSimulation\n";
}

static void TestSurfaceEffector2DConveyorSimulation()
{
	WorldContext context;
	Scene scene(context);

	// 1. 创建传送带地面实体 (SurfaceEffector2DComponent)
	Entity conveyor = scene.CreateEntityShell("Conveyor");
	auto& convT = conveyor.AddComponent<TransformComponent>();
	convT.Location = { 0.0f, 0.0f, 0.0f };
	auto& convRb = conveyor.AddComponent<RigidBody2DComponent>();
	convRb.Type = RigidBody2DComponent::BodyType::Static;
	auto& convBox = conveyor.AddComponent<BoxCollider2DComponent>();
	convBox.Size = { 10.0f, 0.5f };
	convBox.Friction = 0.8f;
	convBox.IsSensor = false;
	auto& surf = conveyor.AddComponent<SurfaceEffector2DComponent>();
	surf.Speed = 5.0f;
	surf.ForceScale = 1.0f;

	// 2. 创建下落接触传送带的物体 (初始位置 Y = 1.2)
	Entity cargo = scene.CreateEntityShell("Cargo");
	auto& cargoT = cargo.AddComponent<TransformComponent>();
	cargoT.Location = { 0.0f, 1.2f, 0.0f };
	auto& cargoRb = cargo.AddComponent<RigidBody2DComponent>();
	cargoRb.Type = RigidBody2DComponent::BodyType::Dynamic;
	auto& cargoBox = cargo.AddComponent<BoxCollider2DComponent>();
	cargoBox.Size = { 0.3f, 0.3f };
	cargoBox.Friction = 0.8f;
	cargoBox.Density = 1.0f;

	scene.OnRuntimeStart();
	assert(scene.IsPhysics2DRunning());

	// 模拟 40 步 (约 0.64s)，物体落在传送带上后在切线速度作用下横向移动 (LinearVelocity.x > 0 且 Location.x > 0)
	for (int i = 0; i < 40; ++i)
	{
		scene.OnFixedUpdate(Timestep(0.016f));
	}

	b2BodyId cargoBody = scene.GetPhysicsBody2D(cargo);
	assert(b2Body_IsValid(cargoBody));
	const b2Vec2 vel = b2Body_GetLinearVelocity(cargoBody);
	assert(vel.x > 0.0f);
	assert(cargoT.Location.x > 0.0f);

	scene.OnRuntimeStop();
	assert(!scene.IsPhysics2DRunning());

	std::cout << "[PASS] TestSurfaceEffector2DConveyorSimulation\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldPhysics2DExtensionsTests...\n";

	TestPolygonDecomposition();
	TestPolygonAndChainPhysicsSimulation();
	TestPlatformEffectorOneWay();
	TestJoints2DComponentPODAndDefaults();
	TestRevoluteJoint2DPendulumSimulation();
	TestDistanceJoint2DSimulation();
	TestPrismaticAndWheelJointsLifecycle();
	TestEffectors2DComponentPODAndDefaults();
	TestAreaEffector2DWindSimulation();
	TestBuoyancyEffector2DFloatingSimulation();
	TestSurfaceEffector2DConveyorSimulation();

	std::cout << "All WorldPhysics2DExtensionsTests passed successfully.\n";
	return 0;
}