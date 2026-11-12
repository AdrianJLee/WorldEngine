#include "World/Physics/PhysicsMaterial.h"
#include <filesystem>
#include "wldpch.h"
#include "World/Core/WorldContext.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"
#include "World/Script/Runtime/ScriptEngine.h"
#include "World/Script/Vm/LuauVm.h"
#include "World/Script/Vm/ScriptValue.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
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

	void RunFail(const std::string& source, const char* chunk, int line)
	{
		std::string error;
		if (ScriptEngine::GetState().RunString(source, chunk, &error))
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + chunk +
				" unexpectedly succeeded");
	}
#define RUN_FAIL(source, chunk) RunFail(source, chunk, __LINE__)
}

int main()
{
	try
	{
		WorldContext context;
		ScriptEngine::Init();

		// 1. 全局只读表 Physics 校验
		RUN_OK(R"(
			assert(type(Physics) == "table", "Physics table must exist")
			assert(type(Physics.Raycast) == "function", "Physics.Raycast must be a function")
			assert(type(Physics.Raycast2D) == "function", "Physics.Raycast2D must be a function")
			assert(type(Physics.SphereCast) == "function", "Physics.SphereCast must be a function")
			assert(type(Physics.OverlapSphere) == "function", "Physics.OverlapSphere must be a function")
			assert(type(Physics.OverlapCircle) == "function", "Physics.OverlapCircle must be a function")
			assert(type(Physics.MoveAndSlide) == "function", "Physics.MoveAndSlide must be a function")
			assert(type(Physics.CanJump) == "function", "Physics.CanJump must be a function")
			assert(type(Physics.TryJump) == "function", "Physics.TryJump must be a function")
			assert(type(Physics.LoadMaterial) == "function", "Physics.LoadMaterial must be a function")
			assert(type(Physics.ApplyMaterial) == "function", "Physics.ApplyMaterial must be a function")
		)", "PhysicsTableCheck");

		// 只读保护测试
		RUN_FAIL(R"(
			Physics.ReadOnlyTest = 123
		)", "PhysicsReadOnlyCheck");

		// 2. 场景与物理探测测试
		Scene scene(context);
		ScriptEngine::SetActiveScene(&scene);

		// 创建 3D 盒体碰撞体
		Entity box3D = Entity::CreateEntity(&scene, "Box3D");
		auto& box3DTr = box3D.AddComponent<TransformComponent>();
		box3DTr.Location = glm::vec3(0.0f, 0.0f, 5.0f);
		auto& box3DComp = box3D.AddComponent<BoxCollider3DComponent>();
		box3DComp.HalfExtents = glm::vec3(1.0f, 1.0f, 1.0f);

		// 创建 2D 盒体碰撞体 (XY 平面)
		Entity box2D = Entity::CreateEntity(&scene, "Box2D");
		auto& box2DTr = box2D.AddComponent<TransformComponent>();
		box2DTr.Location = glm::vec3(5.0f, 0.0f, 0.0f);
		auto& box2DComp = box2D.AddComponent<BoxCollider2DComponent>();
		box2DComp.Size = glm::vec2(1.0f, 1.0f);

		// 创建 CharacterController 实体
		Entity kccEntity = Entity::CreateEntity(&scene, "KCC");
		auto& kccTr = kccEntity.AddComponent<TransformComponent>();
		kccTr.Location = glm::vec3(0.0f, 0.0f, -5.0f);
		auto& kccComp = kccEntity.AddComponent<CharacterControllerComponent>();
		kccComp.Grounded = 1;
		kccComp.CoyoteDuration = 0.12f;
		kccComp.CoyoteTimer = 0.12f;
		auto& kccSphere = kccEntity.AddComponent<SphereCollider3DComponent>();
		kccSphere.Radius = 0.5f;

		uint32_t kccId = static_cast<uint32_t>(static_cast<entt::entity>(kccEntity));
		uint32_t box3DId = static_cast<uint32_t>(static_cast<entt::entity>(box3D));
		uint32_t box2DId = static_cast<uint32_t>(static_cast<entt::entity>(box2D));

		PhysicsMaterial scriptMat;
		scriptMat.Friction = 0.08f;
		scriptMat.Restitution = 0.92f;
		scriptMat.FrictionCombine = PhysMatCombineMode::Minimum;
		scriptMat.RestitutionCombine = PhysMatCombineMode::Maximum;
		scriptMat.SaveToFile("test_script_material.wphysmat");

		std::string setupScript = "KCC_ID = " + std::to_string(kccId) + "\n" +
		                          "BOX3D_ID = " + std::to_string(box3DId) + "\n" +
		                          "BOX2D_ID = " + std::to_string(box2DId) + "\n";
		RUN_OK(setupScript, "SetupEntityIds");

		// 3. Raycast 3D 脚本执行测试
		RUN_OK(R"(
			-- 命中测试
			local hit = Physics.Raycast(0, 0, 0, 0, 0, 1, 10.0)
			assert(hit.Hit == true, "Raycast hit should be true")
			print("[DEBUG Raycast3D] hit.Hit=" .. tostring(hit.Hit) .. " hit.Distance=" .. tostring(hit.Distance))
assert(math.abs(hit.Distance - 4.0) < 0.05, "Raycast distance mismatch")
			assert(math.abs(hit.Point.z - 4.0) < 0.05, "Raycast Point.z mismatch")
			assert(hit.Normal.z < -0.9, "Raycast Normal.z mismatch")
			assert(hit.Entity == BOX3D_ID, "Raycast Entity id mismatch")

			-- 未命中测试
			local miss = Physics.Raycast(0, 0, 0, 0, 1, 0, 10.0)
			assert(miss.Hit == false, "Raycast miss Hit should be false")
			assert(miss.Distance == 0, "Raycast miss Distance should be 0")
		)", "Raycast3DScriptTest");

		// 4. Raycast2D 脚本执行测试
		RUN_OK(R"(
			-- 2D 命中测试 (沿 +X 探测)
			local hit2d = Physics.Raycast2D(0, 0, 1, 0, 10.0)
			assert(hit2d.Hit == true, "Raycast2D hit should be true")
			assert(math.abs(hit2d.Distance - 4.0) < 0.05, "Raycast2D distance mismatch")
			assert(math.abs(hit2d.Point.x - 4.0) < 0.05, "Raycast2D Point.x mismatch")
			assert(hit2d.Normal.x < -0.9, "Raycast2D Normal.x mismatch")
			assert(hit2d.Entity == BOX2D_ID, "Raycast2D Entity id mismatch")

			-- 2D 未命中测试
			local miss2d = Physics.Raycast2D(0, 0, 0, 1, 10.0)
			assert(miss2d.Hit == false, "Raycast2D miss Hit should be false")
		)", "Raycast2DScriptTest");

		// 5. SphereCast 脚本执行测试
		RUN_OK(R"(
			-- 球体扫掠测试 (半径 0.5 沿 +Z 扫掠)
			local scHit = Physics.SphereCast(0, 0, 0, 0.5, 0, 0, 1, 10.0)
			assert(scHit.Hit == true, "SphereCast hit should be true")
			assert(math.abs(scHit.Distance - 3.5) < 0.05, "SphereCast distance mismatch")
			assert(math.abs(scHit.Point.z - 3.5) < 0.05, "SphereCast Point.z mismatch")
			assert(scHit.PenetrationDepth ~= nil, "SphereCast PenetrationDepth should be present")
		)", "SphereCastScriptTest");

		// 6. OverlapSphere 与 OverlapCircle 脚本执行测试
		RUN_OK(R"(
			-- 3D 球体重叠查询
			local overlaps = Physics.OverlapSphere(0, 0, 5, 2.0)
			assert(overlaps.Count >= 1, "OverlapSphere should return >= 1 entity")
			assert(overlaps[1] == BOX3D_ID, "OverlapSphere first element should match Box3D")

			-- 2D 圆形重叠查询
			local overlaps2d = Physics.OverlapCircle(5, 0, 2.0)
			assert(overlaps2d.Count >= 1, "OverlapCircle should return >= 1 entity")
			assert(overlaps2d[1] == BOX2D_ID, "OverlapCircle first element should match Box2D")

			-- 范围外无重叠测试
			local none = Physics.OverlapSphere(100, 100, 100, 1.0)
			assert(none.Count == 0, "Empty OverlapSphere should return 0 count")
		)", "OverlapScriptTest");

		// 7. CanJump 与 TryJump 角色控制器脚本执行测试
		RUN_OK(R"(
			-- 当前处于 Grounded / 土狼时间内，预期 CanJump == true
			local canJump = Physics.CanJump(KCC_ID)
			assert(canJump == true, "CanJump should return true when grounded")

			-- 触发跳跃
			local jumped = Physics.TryJump(KCC_ID, 6.0)
			assert(jumped == true, "TryJump should return true")

			-- 跳跃后 CoyoteTimer 被清空且处于离地状态，CanJump 预期变为 false
			local canJumpAfter = Physics.CanJump(KCC_ID)
			assert(canJumpAfter == false, "CanJump should return false after jumping")
		)", "JumpScriptTest");

		// 8. MoveAndSlide 运动学滑行脚本执行测试
		RUN_OK(R"(
			local disp = Physics.MoveAndSlide(KCC_ID, 1.0, 0.0, 0.0, 0.016)
			assert(type(disp) == "table", "MoveAndSlide must return table")
			assert(type(disp.x) == "number", "MoveAndSlide result x must be number")
			assert(type(disp.y) == "number", "MoveAndSlide result y must be number")
			assert(type(disp.z) == "number", "MoveAndSlide result z must be number")
		)", "MoveAndSlideScriptTest");

		// 8.1. MoveAndSlide2D 角色物理滑动碰撞不变量测试 (对标 Godot move_and_slide)
		RUN_OK(R"(
			-- 静态墙壁放在 x=5.0 处, 宽度 1.0 (左表面在 x=4.5)
			local wall = ecs.CreateBox2D("Wall", 5.0, 0.0, 1.0, 10.0, false)
			-- 玩家放在 x=0.0, 0.0, 尺寸 1.0 (半宽 0.5)
			local player = ecs.CreateBox2D("Player", 0.0, 0.0, 1.0, 1.0, true)

			-- 角色斜向冲撞右侧墙体: vx=10.0 (按无阻挡 0.6 秒将到达 x=6.0 穿透墙壁), vy=5.0 (垂直切向滑动)
			local move = Physics.MoveAndSlide2D(player, 10.0, 5.0, 0.6)
			assert(type(move) == "table", "MoveAndSlide2D must return table")
			assert(move.collided == true, "MoveAndSlide2D must report collision with wall")
			-- 不变量 1: 绝对阻挡防穿模: 玩家右边缘不得穿过 x=4.5 墙壁表面 (玩家中心不得超过 4.01)
			assert(move.x <= 4.05, string.format("Player penetrated wall! move.x=%.3f, expected <= 4.05", move.x))
			-- 不变量 2: 冲向法线垂直分量被消除 (vx 归零或不再向右加速)
			assert(move.vx <= 0.001, string.format("Normal velocity not eliminated! move.vx=%.3f", move.vx))
			-- 不变量 3: 切线滑动保持: Y 轴分量保持并产生正向位移
			assert(move.y > 0.5, string.format("Tangential slide failed! move.y=%.3f", move.y))
		)", "MoveAndSlide2DInvariantTest");

		// 8.2. Tilemap 瓦片地图脚本服务测试 (ecs.CreateTilemap & Tilemap.*)
		RUN_OK(R"(
			local map = ecs.CreateTilemap("TestDungeonMap", 1.0, 1.0, 20, 10, "", true)
			assert(map ~= nil, "CreateTilemap must return entity handle")

			local setOk = Tilemap.SetTile(map, 3, 4, 7, true)
			assert(setOk == true, "Tilemap.SetTile should succeed")
			assert(Tilemap.GetTile(map, 3, 4) == 7, "Tilemap.GetTile should return 7")
			assert(Tilemap.IsSolid(map, 3, 4) == true, "Tilemap.IsSolid should return true")

			local fillOk = Tilemap.Fill(map, 0, 0, 5, 5, 2, false)
			assert(fillOk == true, "Tilemap.Fill should succeed")
			assert(Tilemap.GetTile(map, 2, 2) == 2, "Tilemap.GetTile in fill region should be 2")
			assert(Tilemap.IsSolid(map, 2, 2) == false, "Tilemap.IsSolid in fill region should be false")
		)", "TilemapServiceTest");

		// 8.3. 2D 摄像机屏幕与世界坐标双向对齐不变量测试 (Camera.ScreenToWorld2D / WorldToScreen2D)
		RUN_OK(R"(
			local cam2d = ecs.CreateCamera2D("MainCam2D", 0.0, 0.0, 5.0, true)
			assert(cam2d ~= nil, "CreateCamera2D must return entity")

			-- 屏幕中心 (640, 360) 必须精确对齐世界坐标 (0.0, 0.0)
			local centerWorld = Camera.ScreenToWorld2D(640.0, 360.0)
			assert(math.abs(centerWorld.x) < 0.001, string.format("Center World X mismatch: %.4f", centerWorld.x))
			assert(math.abs(centerWorld.y) < 0.001, string.format("Center World Y mismatch: %.4f", centerWorld.y))

			-- 任意 2D 世界点往返一致性测试: World -> Screen -> World 误差 < 0.001
			local testWorldX = 2.5
			local testWorldY = -1.8
			local screenPos = Camera.WorldToScreen2D(testWorldX, testWorldY)
			local roundTrip = Camera.ScreenToWorld2D(screenPos.x, screenPos.y)
			assert(math.abs(roundTrip.x - testWorldX) < 0.001, "RoundTrip X mismatch")
			assert(math.abs(roundTrip.y - testWorldY) < 0.001, "RoundTrip Y mismatch")
		)", "Camera2DCoordinateParityTest");

		// 8.5. 物理材质加载与应用测试
		RunOk(R"lua(
			local mat = Physics.LoadMaterial("test_script_material.wphysmat")
			assert(type(mat) == "table", "Physics.LoadMaterial should return table")
			assert(math.abs(mat.Friction - 0.08) < 0.01, "Material Friction mismatch")
			assert(math.abs(mat.Restitution - 0.92) < 0.01, "Material Restitution mismatch")
			assert(mat.FrictionCombine == 1, "FrictionCombine mismatch (1=Minimum)")
			assert(mat.RestitutionCombine == 3, "RestitutionCombine mismatch (3=Maximum)")

			local applied = Physics.ApplyMaterial(BOX3D_ID, "test_script_material.wphysmat")
			assert(applied == true, "ApplyMaterial should succeed")

			local appliedTbl = Physics.ApplyMaterial(BOX2D_ID, {
				Friction = 0.35,
				Restitution = 0.5,
				FrictionCombine = 2,
				RestitutionCombine = 0
			})
			assert(appliedTbl == true, "ApplyMaterial with table should succeed")
		)lua", "PhysicsMaterialScriptTest", __LINE__);

		std::filesystem::remove("test_script_material.wphysmat");

		// 9. 参数错误与边界校验
		RUN_FAIL(R"(
			Physics.Raycast(1, 2)
		)", "InsufficientArgsRaycast");

		RUN_FAIL(R"(
			Physics.TryJump(KCC_ID)
		)", "InsufficientArgsTryJump");

		ScriptEngine::SetActiveScene(nullptr);
		ScriptEngine::Shutdown();

		std::printf("World.PhysicsScript: all checks passed\n");
		return 0;
	}
	catch (const std::exception& ex)
	{
		std::fprintf(stderr, "World.PhysicsScript: fatal: %s\n", ex.what());
		return 1;
	}
}
