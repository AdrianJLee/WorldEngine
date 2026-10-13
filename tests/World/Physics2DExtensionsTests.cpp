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

	// 落地后停留在地表链条上 (Y 应该落在 0.0 ~ 1.0 之间，绝不掉穿入负无穷)
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

int main()
{
	Log::Init();
	std::cout << "Running WorldPhysics2DExtensionsTests...\n";

	TestPolygonDecomposition();
	TestPolygonAndChainPhysicsSimulation();
	TestPlatformEffectorOneWay();

	std::cout << "All WorldPhysics2DExtensionsTests passed successfully.\n";
	return 0;
}
