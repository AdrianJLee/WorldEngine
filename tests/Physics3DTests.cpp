// P1b D6:3D 物理(Jolt)headless 回归。
//
// 覆盖 plan §D6 冻结决定的 9 条验收:
//   ① 自由落体(1s 下降符合 g)          ② 两箱堆叠不穿插           ③ 静态体不动
//   ④ Kinematic 跟随 Transform           ⑤ 凸包由 .wmodel 顶点构建   ⑥ 2D+3D 共存互不干扰
//   ⑦ 接触回调(箱落地触发 added)         ⑧ DebugLines 线段数        ⑨ 同实体双物理拒绝
// 外加脚手架基线段:RigidBody2DComponent 仍在(共存用例依赖它)。
#include "wldpch.h"
#include "World/Core/Asset/WModelIO.h"
#include "World/Core/WorldContext.h"
#include "World/Physics/Physics3D.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <box2d/box2d.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
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

	Entity AddBox(Scene& scene, const char* name, const glm::vec3& location, RigidBody3DComponent::MotionType type,
		const glm::vec3& halfExtents)
	{
		Entity entity = AddEntityAt(scene, name, location);
		RigidBody3DComponent& rigidBody = entity.AddComponent<RigidBody3DComponent>();
		rigidBody.Type = type;
		rigidBody.Restitution = 0.0f;   // 测试里不要弹跳,便于稳定断言
		entity.AddComponent<BoxCollider3DComponent>().HalfExtents = halfExtents;
		return entity;
	}

	void StepWorld(Physics3DWorld& world, int frames)
	{
		for (int frame = 0; frame < frames; ++frame)
			world.Step(kFixedStep);
	}

	// 8 顶点 / 12 三角形的最小闭合立方体 .wmodel(凸包与三角网格两种模式共用)。
	Asset::WModelData MakeCubeModel(float halfExtent)
	{
		Asset::WModelData model;
		const glm::vec3 corners[8] = {
			{ -halfExtent, -halfExtent, -halfExtent }, { halfExtent, -halfExtent, -halfExtent },
			{ halfExtent, halfExtent, -halfExtent }, { -halfExtent, halfExtent, -halfExtent },
			{ -halfExtent, -halfExtent, halfExtent }, { halfExtent, -halfExtent, halfExtent },
			{ halfExtent, halfExtent, halfExtent }, { -halfExtent, halfExtent, halfExtent },
		};
		for (const glm::vec3& corner : corners)
			model.Vertices.push_back({ corner, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f) });

		const uint32_t indices[36] = {
			4, 5, 6, 4, 6, 7,   // +Z
			1, 0, 3, 1, 3, 2,   // -Z
			0, 4, 7, 0, 7, 3,   // -X
			5, 1, 2, 5, 2, 6,   // +X
			0, 1, 5, 0, 5, 4,   // -Y
			3, 7, 6, 3, 6, 2,   // +Y
		};
		for (const uint32_t index : indices)
			model.Indices.push_back(index);

		model.Bounds.Min = glm::vec3(-halfExtent);
		model.Bounds.Max = glm::vec3(halfExtent);
		Asset::WModelSubmesh submesh;
		submesh.IndexOffset = 0;
		submesh.IndexCount = 36;
		submesh.MaterialSlot = -1;
		submesh.Bounds = model.Bounds;
		model.Submeshes.push_back(submesh);
		model.Meshes.push_back({ 0, 1 });
		return model;
	}

	// 0.D6 前置:2D 物理组件仍在(2D/3D 共存这条验收依赖它)。
	void RigidBody2DComponentStillExists()
	{
		CHECK(sizeof(RigidBody2DComponent) > 0);
	}

	// 1.自由落体:1s 后 y 下降符合 g(kGravity = -9.81;固定步 60 Hz 半隐式欧拉,允许离散误差)。
	void FreeFallMatchesGravity()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity faller = AddBox(*scene, "Faller", { 0.0f, 10.0f, 0.0f },
			RigidBody3DComponent::MotionType::Dynamic, { 0.5f, 0.5f, 0.5f });

		Physics3DWorld world;
		world.Start(*scene);
		StepWorld(world, 60);
		world.SyncTransforms();

		glm::vec3 location { 0.0f };
		glm::quat rotation { 1.0f, 0.0f, 0.0f, 0.0f };
		CHECK(world.TryGetBodyTransform(faller, &location, &rotation));
		const float drop = 10.0f - location.y;
		std::printf("[info] free fall 1s: y=%.4f drop=%.4f (analytic 4.905)\n", location.y, drop);
		CHECK(drop > 4.5f && drop < 5.3f);
		CHECK(location.y < 10.0f);
		CHECK(std::fabs(location.x) < 1e-4f && std::fabs(location.z) < 1e-4f);

		// Step 后的写回:TransformComponent 与 Jolt 权威位姿一致;位姿没变时不再写(避免每帧标脏)。
		const TransformComponent& transform = faller.GetComponent<TransformComponent>();
		CHECK(std::fabs(transform.Location.y - location.y) < 1e-4f);
		CHECK(!scene->SyncPhysics3DTransform(faller, location, rotation));

		world.Stop();
		CHECK(!world.IsStarted());
	}

	// 2.两箱堆叠不穿插:静态地面 + 两个动态箱,5s 后第二箱停稳在第一箱顶面(中心间距 ≈ 1.0)。
	void StackedBoxesDoNotInterpenetrate()
	{
		Ref<Scene> scene = CreateTestScene();
		AddBox(*scene, "Ground", { 0.0f, -0.5f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 5.0f, 0.5f, 5.0f });
		Entity lower = AddBox(*scene, "Lower", { 0.0f, 0.6f, 0.0f },
			RigidBody3DComponent::MotionType::Dynamic, { 0.5f, 0.5f, 0.5f });
		Entity upper = AddBox(*scene, "Upper", { 0.0f, 2.0f, 0.0f },
			RigidBody3DComponent::MotionType::Dynamic, { 0.5f, 0.5f, 0.5f });

		Physics3DWorld world;
		world.Start(*scene);
		StepWorld(world, 300);
		world.SyncTransforms();

		glm::vec3 lowerLocation { 0.0f };
		glm::vec3 upperLocation { 0.0f };
		CHECK(world.TryGetBodyTransform(lower, &lowerLocation, nullptr));
		CHECK(world.TryGetBodyTransform(upper, &upperLocation, nullptr));
		const float gap = upperLocation.y - lowerLocation.y;
		std::printf("[info] stack: lower.y=%.4f upper.y=%.4f gap=%.4f\n", lowerLocation.y, upperLocation.y, gap);
		CHECK(lowerLocation.y > 0.4f && lowerLocation.y < 0.6f);   // 地面顶面 y=0,箱半高 0.5
		CHECK(gap > 0.9f && gap < 1.1f);                          // 不穿插、也没有悬空台阶
		world.Stop();
	}

	// 3.静态体不动:120 帧后 Jolt 位姿与 TransformComponent 都保持原值(Static 不参与写回)。
	void StaticBodyDoesNotMove()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity box = AddBox(*scene, "Static", { 1.0f, 2.0f, 3.0f },
			RigidBody3DComponent::MotionType::Static, { 1.0f, 0.5f, 2.0f });

		Physics3DWorld world;
		world.Start(*scene);
		StepWorld(world, 120);
		world.SyncTransforms();

		glm::vec3 location { 0.0f };
		CHECK(world.TryGetBodyTransform(box, &location, nullptr));
		CHECK(std::fabs(location.x - 1.0f) < 1e-6f);
		CHECK(std::fabs(location.y - 2.0f) < 1e-6f);
		CHECK(std::fabs(location.z - 3.0f) < 1e-6f);
		const TransformComponent& transform = box.GetComponent<TransformComponent>();
		CHECK(std::fabs(transform.Location.x - 1.0f) < 1e-6f);
		CHECK(std::fabs(transform.Location.y - 2.0f) < 1e-6f);
		CHECK(std::fabs(transform.Location.z - 3.0f) < 1e-6f);
		std::printf("[info] static body stays at (%.4f, %.4f, %.4f)\n", location.x, location.y, location.z);
		world.Stop();
	}

	// 4.Kinematic 跟随 Transform:每帧把 Transform 往 +X 挪 0.05,40 帧后刚体 x ≈ 2.0。
	void KinematicFollowsTransform()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity mover = AddBox(*scene, "Mover", { 0.0f, 1.0f, 0.0f },
			RigidBody3DComponent::MotionType::Kinematic, { 0.5f, 0.5f, 0.5f });

		Physics3DWorld world;
		world.Start(*scene);
		for (int frame = 0; frame < 40; ++frame)
		{
			TransformComponent& transform = mover.GetComponent<TransformComponent>();
			transform.SetLocation(transform.Location + glm::vec3(0.05f, 0.0f, 0.0f));
			world.Step(kFixedStep);
			world.SyncTransforms();
		}

		glm::vec3 location { 0.0f };
		CHECK(world.TryGetBodyTransform(mover, &location, nullptr));
		std::printf("[info] kinematic after 40 frames: x=%.4f y=%.4f\n", location.x, location.y);
		CHECK(std::fabs(location.x - 2.0f) < 0.05f);
		CHECK(std::fabs(location.y - 1.0f) < 0.05f);
		const TransformComponent& transform = mover.GetComponent<TransformComponent>();
		CHECK(std::fabs(transform.Location.x - 2.0f) < 0.05f);
		world.Stop();
	}

	// 5.凸包由 .wmodel 顶点构建:动态凸包落到地面顶面(y ≈ 0.5);StaticTriangles 挂动态刚体被拒绝。
	void ConvexHullFromWModel()
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path() / "we-physics3d-tests";
		std::filesystem::create_directories(directory);
		const std::filesystem::path modelPath = directory / "D6Cube.wmodel";
		std::string error;
		CHECK(Asset::WModelIO::WriteFile(modelPath.string(), MakeCubeModel(0.5f), &error));

		Ref<Scene> scene = CreateTestScene();
		AddBox(*scene, "Ground", { 0.0f, -0.5f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 5.0f, 0.5f, 5.0f });
		Entity hull = AddEntityAt(*scene, "Hull", { 0.0f, 3.0f, 0.0f });
		RigidBody3DComponent& body = hull.AddComponent<RigidBody3DComponent>();
		body.Type = RigidBody3DComponent::MotionType::Dynamic;
		body.Restitution = 0.0f;
		MeshCollider3DComponent& collider = hull.AddComponent<MeshCollider3DComponent>();
		collider.Mode = MeshCollider3DComponent::ColliderMode::ConvexHull;
		collider.MeshPath = modelPath.string();

		Physics3DWorld world;
		world.Start(*scene);
		StepWorld(world, 180);
		glm::vec3 location { 0.0f };
		CHECK(world.TryGetBodyTransform(hull, &location, nullptr));
		std::printf("[info] convex hull landed at y=%.4f (model %s)\n", location.y,
			modelPath.filename().string().c_str());
		CHECK(location.y > 0.4f && location.y < 0.7f);
		world.Stop();

		// StaticTriangles 只允许静态刚体 → 校验/启动都拒绝(可读错误)。
		Ref<Scene> invalid = CreateTestScene();
		Entity bad = AddEntityAt(*invalid, "BadTriangles", { 0.0f, 0.0f, 0.0f });
		RigidBody3DComponent& badBody = bad.AddComponent<RigidBody3DComponent>();
		badBody.Type = RigidBody3DComponent::MotionType::Dynamic;
		MeshCollider3DComponent& triangles = bad.AddComponent<MeshCollider3DComponent>();
		triangles.Mode = MeshCollider3DComponent::ColliderMode::StaticTriangles;
		triangles.MeshPath = modelPath.string();
		CHECK(!Physics3DWorld::ValidateScene(*invalid, &error));
		CHECK(error.find("StaticTriangles") != std::string::npos);
		CHECK(error.find("Static") != std::string::npos);
		Physics3DWorld rejecting;
		bool threw = false;
		try { rejecting.Start(*invalid); }
		catch (const std::logic_error& exception) { threw = std::string(exception.what()).find("StaticTriangles") != std::string::npos; }
		CHECK(threw);
		CHECK(!rejecting.IsStarted());
	}

	// 6.2D+3D 共存互不干扰:同场景两套世界各自步进(下落量一致、各自轴不动)。
	void Physics2DAnd3DCoexist()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity faller2D = AddEntityAt(*scene, "Faller2D", { 0.0f, 10.0f, 0.0f });
		RigidBody2DComponent& body2D = faller2D.AddComponent<RigidBody2DComponent>();
		body2D.Type = RigidBody2DComponent::BodyType::Dynamic;
		faller2D.AddComponent<BoxCollider2DComponent>().Size = { 0.5f, 0.5f };

		Entity faller3D = AddBox(*scene, "Faller3D", { 10.0f, 10.0f, 0.0f },
			RigidBody3DComponent::MotionType::Dynamic, { 0.5f, 0.5f, 0.5f });

		scene->OnRuntimeStart();
		CHECK(scene->IsPhysics2DRunning());
		CHECK(scene->IsPhysics3DRunning());
		for (int frame = 0; frame < 60; ++frame)
			scene->OnScriptUpdate(Timestep(kFixedStep));   // 2D/3D 同一条固定步路径

		const b2Vec2 position2D = b2Body_GetPosition(faller2D.GetComponent<RigidBody2DComponent>().RuntimeBodyId);
		glm::vec3 position3D { 0.0f };
		CHECK(scene->GetPhysics3DWorld() != nullptr);
		CHECK(scene->GetPhysics3DWorld()->TryGetBodyTransform(faller3D, &position3D, nullptr));

		const float drop2D = 10.0f - position2D.y;
		const float drop3D = 10.0f - position3D.y;
		std::printf("[info] coexist 1s: 2D=%.4f 3D=%.4f (drop 2D=%.4f 3D=%.4f)\n",
			position2D.y, position3D.y, drop2D, drop3D);
		CHECK(drop2D > 4.5f && drop2D < 5.3f);
		CHECK(drop3D > 4.5f && drop3D < 5.3f);
		CHECK(std::fabs(drop2D - drop3D) < 0.2f);       // 两套世界互不干扰
		CHECK(std::fabs(position2D.x) < 1e-3f);         // 2D 世界看不到 3D 物体
		CHECK(std::fabs(position3D.x - 10.0f) < 1e-3f); // 3D 世界看不到 2D 物体
		CHECK(std::fabs(faller3D.GetComponent<TransformComponent>().Location.y - position3D.y) < 1e-4f);

		scene->OnRuntimeStop();
		CHECK(!scene->IsPhysics2DRunning());
		CHECK(!scene->IsPhysics3DRunning());
		CHECK(scene->GetPhysics3DWorld() == nullptr);
	}

	// 7.接触回调:箱落到地面触发 added(实体对匹配);Scene 钩子表转发同一条事件。
	void ContactCallbackFiresOnLanding()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity ground = AddBox(*scene, "Ground", { 0.0f, -0.5f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 5.0f, 0.5f, 5.0f });
		Entity faller = AddBox(*scene, "Faller", { 0.0f, 2.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic, { 0.5f, 0.5f, 0.5f });
		const entt::entity groundHandle = ground;
		const entt::entity fallerHandle = faller;

		Physics3DWorld world;
		int added = 0;
		int removed = 0;
		bool pairMatched = false;
		world.SetContactCallback([&](bool isAdded, entt::entity entityA, entt::entity entityB)
		{
			if (isAdded)
			{
				++added;
				if ((entityA == groundHandle && entityB == fallerHandle) || (entityA == fallerHandle && entityB == groundHandle))
					pairMatched = true;
			}
			else
			{
				++removed;
			}
		});
		world.Start(*scene);
		StepWorld(world, 150);
		std::printf("[info] contact: added=%d removed=%d pairMatched=%d\n", added, removed, pairMatched ? 1 : 0);
		CHECK(added >= 1);
		CHECK(pairMatched);
		world.Stop();

		// Scene 转发:同一布局走 Scene::AddPhysics3DContactCallback + OnScriptUpdate。
		Ref<Scene> hooked = CreateTestScene();
		Entity groundHook = AddBox(*hooked, "Ground", { 0.0f, -0.5f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 5.0f, 0.5f, 5.0f });
		Entity fallerHook = AddBox(*hooked, "Faller", { 0.0f, 2.0f, 0.0f }, RigidBody3DComponent::MotionType::Dynamic, { 0.5f, 0.5f, 0.5f });
		const entt::entity groundHookHandle = groundHook;
		const entt::entity fallerHookHandle = fallerHook;
		int hookEvents = 0;
		bool hookPairMatched = false;
		hooked->AddPhysics3DContactCallback([&](bool isAdded, entt::entity entityA, entt::entity entityB)
		{
			if (!isAdded) return;
			++hookEvents;
			if ((entityA == groundHookHandle && entityB == fallerHookHandle) || (entityA == fallerHookHandle && entityB == groundHookHandle))
				hookPairMatched = true;
		});
		hooked->OnRuntimeStart();
		for (int frame = 0; frame < 150; ++frame)
			hooked->OnScriptUpdate(Timestep(kFixedStep));
		hooked->OnRuntimeStop();
		std::printf("[info] scene hook events=%d pairMatched=%d\n", hookEvents, hookPairMatched ? 1 : 0);
		CHECK(hookEvents >= 1);
		CHECK(hookPairMatched);
		CHECK(!hooked->IsPhysics3DRunning());
	}

	// 8.DebugLines:box 12 棱 / sphere 3 圆(24 段) / capsule 2 圆 + 4 侧线;世界空间坐标正确。
	void DebugLinesCoverAllColliders()
	{
		// 单一 box:12 条棱;包围盒 = 中心 ± HalfExtents(世界空间,body 没动)。
		{
			Ref<Scene> boxScene = CreateTestScene();
			AddBox(*boxScene, "Box", { 0.0f, 0.0f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 1.0f, 2.0f, 3.0f });
			Physics3DWorld boxWorld;
			boxWorld.Start(*boxScene);
			std::vector<DebugLine> boxLines;
			boxWorld.CollectDebugLines(boxLines);
			CHECK(boxLines.size() == 12u);
			glm::vec3 minimum(std::numeric_limits<float>::max());
			glm::vec3 maximum(std::numeric_limits<float>::lowest());
			for (const DebugLine& line : boxLines)
			{
				minimum = glm::min(minimum, glm::min(line.Begin, line.End));
				maximum = glm::max(maximum, glm::max(line.Begin, line.End));
			}
			CHECK(glm::distance(minimum, glm::vec3(-1.0f, -2.0f, -3.0f)) < 1e-4f);
			CHECK(glm::distance(maximum, glm::vec3(1.0f, 2.0f, 3.0f)) < 1e-4f);
			boxWorld.Stop();
		}

		// box + sphere + capsule:总线段数 = 12 + 3×24 + (2×24 + 4)。
		Ref<Scene> scene = CreateTestScene();
		AddBox(*scene, "Box", { 0.0f, 0.0f, 0.0f }, RigidBody3DComponent::MotionType::Static, { 1.0f, 2.0f, 3.0f });

		Entity sphere = AddEntityAt(*scene, "Sphere", { 10.0f, 0.0f, 0.0f });
		sphere.AddComponent<RigidBody3DComponent>();
		sphere.AddComponent<SphereCollider3DComponent>().Radius = 1.0f;

		Entity capsule = AddEntityAt(*scene, "Capsule", { 20.0f, 0.0f, 0.0f });
		capsule.AddComponent<RigidBody3DComponent>();
		RigidBody3DComponent& kinematic = capsule.GetComponent<RigidBody3DComponent>();
		kinematic.Type = RigidBody3DComponent::MotionType::Kinematic;
		CapsuleCollider3DComponent& capsuleCollider = capsule.AddComponent<CapsuleCollider3DComponent>();
		capsuleCollider.Radius = 0.5f;
		capsuleCollider.HalfHeight = 1.0f;

		Physics3DWorld world;
		world.Start(*scene);
		std::vector<DebugLine> lines;
		world.CollectDebugLines(lines);
		std::printf("[info] debug lines: total=%zu (box 12 + sphere 72 + capsule 52)\n", lines.size());
		CHECK(lines.size() == 12u + 72u + 52u);
		for (const DebugLine& line : lines)
		{
			CHECK(std::isfinite(line.Begin.x) && std::isfinite(line.End.x));
			CHECK(glm::distance(line.Begin, line.End) > 1e-4f);
		}
		world.Stop();

		// 未启动 → 可读错误(不静默)。
		std::vector<DebugLine> unused;
		bool threw = false;
		try { world.CollectDebugLines(unused); }
		catch (const std::logic_error& exception) { threw = std::string(exception.what()).find("Start(Scene&)") != std::string::npos; }
		CHECK(threw);
	}

	// 9.同实体双物理拒绝:2D+3D 组件混挂 → Start 与 Scene::OnRuntimeStart 都拒绝,场景保持 Stopped。
	void DualPhysicsOnSameEntityIsRejected()
	{
		Ref<Scene> scene = CreateTestScene();
		Entity conflicted = AddEntityAt(*scene, "Conflicted", { 0.0f, 0.0f, 0.0f });
		conflicted.AddComponent<RigidBody2DComponent>();
		RigidBody3DComponent& body3D = conflicted.AddComponent<RigidBody3DComponent>();
		body3D.Type = RigidBody3DComponent::MotionType::Dynamic;
		conflicted.AddComponent<BoxCollider3DComponent>();
		const std::string entityLabel = std::to_string(static_cast<uint32_t>(static_cast<entt::entity>(conflicted)));

		std::string error;
		CHECK(!Physics3DWorld::ValidateScene(*scene, &error));
		CHECK(error.find("2D") != std::string::npos);
		CHECK(error.find("3D") != std::string::npos);
		CHECK(error.find(entityLabel) != std::string::npos);
		std::printf("[info] dual physics rejection: %s\n", error.c_str());

		Physics3DWorld world;
		bool threw = false;
		try { world.Start(*scene); }
		catch (const std::logic_error& exception) { threw = std::string(exception.what()).find("2D and 3D") != std::string::npos; }
		CHECK(threw);
		CHECK(!world.IsStarted());

		threw = false;
		try { scene->OnRuntimeStart(); }
		catch (const std::logic_error& exception) { threw = std::string(exception.what()).find("2D and 3D") != std::string::npos; }
		CHECK(threw);
		CHECK(!scene->IsActive());
		CHECK(!scene->IsPhysics2DRunning());   // 校验在创建任何物理世界之前 → 没有半启动的 2D 世界
		CHECK(!scene->IsPhysics3DRunning());
	}
}

int main()
{
	try
	{
		std::setvbuf(stdout, nullptr, _IONBF, 0);

		const std::pair<const char*, void(*)()> tests[] = {
			{ "RigidBody2DComponent still exists (2D/3D coexistence baseline)", RigidBody2DComponentStillExists },
			{ "free fall matches gravity (1s drop ~ g)", FreeFallMatchesGravity },
			{ "stacked boxes do not interpenetrate", StackedBoxesDoNotInterpenetrate },
			{ "static body does not move", StaticBodyDoesNotMove },
			{ "kinematic body follows TransformComponent", KinematicFollowsTransform },
			{ "convex hull from .wmodel + StaticTriangles requires static", ConvexHullFromWModel },
			{ "2D and 3D physics coexist without interference", Physics2DAnd3DCoexist },
			{ "contact callback fires on landing (world + Scene hook)", ContactCallbackFiresOnLanding },
			{ "CollectDebugLines covers box/sphere/capsule", DebugLinesCoverAllColliders },
			{ "2D+3D on one entity is rejected at runtime", DualPhysicsOnSameEntityIsRejected },
		};
		int failures = 0;
		for (const auto& [name, test] : tests)
		{
			try { test(); std::printf("[PASS] %s\n", name); }
			catch (const std::exception& error) { ++failures; std::fprintf(stderr, "[FAIL] %s: %s\n", name, error.what()); }
			catch (...) { ++failures; std::fprintf(stderr, "[FAIL] %s: unknown exception\n", name); }
		}
		if (failures == 0)
			std::printf("World.Physics3D: all checks passed\n");
		else
			std::fprintf(stderr, "World.Physics3D: %d group(s) failed\n", failures);
		return failures == 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Physics3D: fatal: %s\n", error.what());
		return 1;
	}
}
