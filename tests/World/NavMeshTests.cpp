#include "World/Core/WorldContext.h"
#include "World/NavMesh/NavMesh.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"
#include "World/Level/Authoring/LevelPackage.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

using namespace World;

static void TestNavMeshPolygonBuildingAndAdjacency()
{
	NavMesh nav;

	// 创建一个 2x2 的平面网格 (4 个四边形多边形)
	// (0,0)---(10,0)---(20,0)
	//   |        |        |
	// (0,10)--(10,10)--(20,10)
	//   |        |        |
	// (0,20)--(10,20)--(20,20)

	uint32_t v[3][3];
	for (int z = 0; z < 3; ++z)
		for (int x = 0; x < 3; ++x)
			v[z][x] = nav.AddVertex(glm::vec3(x * 10.0f, 0.0f, z * 10.0f));

	uint32_t p0 = nav.AddPolygon({ v[0][0], v[0][1], v[1][1], v[1][0] });
	uint32_t p1 = nav.AddPolygon({ v[0][1], v[0][2], v[1][2], v[1][1] });
	uint32_t p2 = nav.AddPolygon({ v[1][0], v[1][1], v[2][1], v[2][0] });
	uint32_t p3 = nav.AddPolygon({ v[1][1], v[1][2], v[2][2], v[2][1] });

	nav.BuildAdjacency();

	assert(nav.GetPolygonCount() == 4);
	// 验证 p0 与 p1 邻接，p0 与 p2 邻接
	const auto& poly0 = nav.GetPolygon(p0);
	bool foundP1 = false, foundP2 = false;
	for (int32_t n : poly0.Neighbors)
	{
		if (n == static_cast<int32_t>(p1)) foundP1 = true;
		if (n == static_cast<int32_t>(p2)) foundP2 = true;
	}
	assert(foundP1);
	assert(foundP2);

	std::cout << "[PASS] TestNavMeshPolygonBuildingAndAdjacency\n";
}

static void TestNavMeshSamplePosition()
{
	NavMesh nav;
	uint32_t v0 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 0.0f));
	uint32_t v1 = nav.AddVertex(glm::vec3(10.0f, 0.0f, 0.0f));
	uint32_t v2 = nav.AddVertex(glm::vec3(10.0f, 0.0f, 10.0f));
	uint32_t v3 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 10.0f));
	nav.AddPolygon({ v0, v1, v2, v3 });

	glm::vec3 navPt;
	uint32_t polyIdx = 0;
	// 在多边形正上方 5m 投影
	bool ok = nav.SamplePosition(glm::vec3(5.0f, 5.0f, 5.0f), 20.0f, navPt, polyIdx);
	assert(ok);
	assert(polyIdx == 0);
	assert(std::abs(navPt.x - 5.0f) < 1e-4f);
	assert(std::abs(navPt.y - 0.0f) < 1e-4f);
	assert(std::abs(navPt.z - 5.0f) < 1e-4f);

	std::cout << "[PASS] TestNavMeshSamplePosition\n";
}

static void TestNavMeshFindPathAndFunnel()
{
	NavMesh nav;
	// 建立 3 个连续四边形形成的通道: (0,0) -> (10,0) -> (20,0) -> (30,0)
	uint32_t v0 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 0.0f));
	uint32_t v1 = nav.AddVertex(glm::vec3(10.0f, 0.0f, 0.0f));
	uint32_t v2 = nav.AddVertex(glm::vec3(20.0f, 0.0f, 0.0f));
	uint32_t v3 = nav.AddVertex(glm::vec3(30.0f, 0.0f, 0.0f));

	uint32_t v4 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 10.0f));
	uint32_t v5 = nav.AddVertex(glm::vec3(10.0f, 0.0f, 10.0f));
	uint32_t v6 = nav.AddVertex(glm::vec3(20.0f, 0.0f, 10.0f));
	uint32_t v7 = nav.AddVertex(glm::vec3(30.0f, 0.0f, 10.0f));

	nav.AddPolygon({ v0, v1, v5, v4 });
	nav.AddPolygon({ v1, v2, v6, v5 });
	nav.AddPolygon({ v2, v3, v7, v6 });
	nav.BuildAdjacency();

	std::vector<glm::vec3> path;
	bool ok = nav.FindPath(glm::vec3(2.0f, 0.0f, 5.0f), glm::vec3(28.0f, 0.0f, 5.0f), path);
	assert(ok);
	assert(path.size() >= 2);
	// 起点与终点 X 必须平滑单调递增
	assert(path.front().x <= 2.1f);
	assert(path.back().x >= 27.9f);

	std::cout << "[PASS] TestNavMeshFindPathAndFunnel (Waypoints: " << path.size() << ")\n";
}

static void TestNavMeshDynamicObstacleCarving()
{
	NavMesh nav;
	// (0,0) 到 (30,10) 的 3 个连通多边形
	uint32_t v[8];
	v[0] = nav.AddVertex(glm::vec3(0.0f, 0.0f, 0.0f));
	v[1] = nav.AddVertex(glm::vec3(10.0f, 0.0f, 0.0f));
	v[2] = nav.AddVertex(glm::vec3(20.0f, 0.0f, 0.0f));
	v[3] = nav.AddVertex(glm::vec3(30.0f, 0.0f, 0.0f));

	v[4] = nav.AddVertex(glm::vec3(0.0f, 0.0f, 10.0f));
	v[5] = nav.AddVertex(glm::vec3(10.0f, 0.0f, 10.0f));
	v[6] = nav.AddVertex(glm::vec3(20.0f, 0.0f, 10.0f));
	v[7] = nav.AddVertex(glm::vec3(30.0f, 0.0f, 10.0f));

	nav.AddPolygon({ v[0], v[1], v[5], v[4] }); // 0
	nav.AddPolygon({ v[1], v[2], v[6], v[5] }); // 1 (中间)
	nav.AddPolygon({ v[2], v[3], v[7], v[6] }); // 2
	nav.BuildAdjacency();

	// 在中间多边形放置动态障碍物，打洞阻断
	nav.ApplyObstacle(glm::vec3(15.0f, 0.0f, 5.0f), glm::vec3(4.0f, 2.0f, 4.0f), true);

	std::vector<glm::vec3> path;
	bool blocked = nav.FindPath(glm::vec3(2.0f, 0.0f, 5.0f), glm::vec3(28.0f, 0.0f, 5.0f), path);
	assert(!blocked); // 无法通行

	// 移除障碍物，恢复通行
	nav.ClearObstacles();
	bool restored = nav.FindPath(glm::vec3(2.0f, 0.0f, 5.0f), glm::vec3(28.0f, 0.0f, 5.0f), path);
	assert(restored);

	std::cout << "[PASS] TestNavMeshDynamicObstacleCarving\n";
}

static void TestNavMeshTerrainBaking()
{
	TerrainData terrain(32, 32, 2.0f, 1.0f);
	// 制造中央平原与四周陡崖
	for (uint32_t z = 0; z < 32; ++z)
	{
		for (uint32_t x = 0; x < 32; ++x)
		{
			if (x > 28 || z > 28)
				terrain.SetHeight(x, z, 50.0f); // 陡峭悬崖
			else
				terrain.SetHeight(x, z, 0.0f);  // 平原
		}
	}

	NavMesh nav;
	nav.BakeFromTerrain(terrain, 45.0f, 4.0f);
	assert(nav.GetPolygonCount() > 0);
	assert(nav.GetVertexCount() > 0);

	// 平原上可以顺利规划路径
	std::vector<glm::vec3> path;
	bool ok = nav.FindPath(glm::vec3(4.0f, 0.0f, 4.0f), glm::vec3(20.0f, 0.0f, 20.0f), path);
	assert(ok);

	std::cout << "[PASS] TestNavMeshTerrainBaking (Polygons: " << nav.GetPolygonCount() << ")\n";
}

static void TestNavMeshAgentMovementAndArrival()
{
	NavMesh nav;
	uint32_t v0 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 0.0f));
	uint32_t v1 = nav.AddVertex(glm::vec3(20.0f, 0.0f, 0.0f));
	uint32_t v2 = nav.AddVertex(glm::vec3(20.0f, 0.0f, 20.0f));
	uint32_t v3 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 20.0f));
	nav.AddPolygon({ v0, v1, v2, v3 });
	nav.BuildAdjacency();

	WorldContext context;
	Scene scene(context);
	const entt::entity entity = scene.GetRegistry().create();
	auto& xform = scene.GetRegistry().emplace<TransformComponent>(entity);
	xform.Location = glm::vec3(2.0f, 0.0f, 2.0f);

	auto& agent = scene.GetRegistry().emplace<NavMeshAgentComponent>(entity);
	agent.Destination = glm::vec3(10.0f, 0.0f, 2.0f); // 8m 距离
	agent.Speed = 5.0f;                               // 5m/s
	agent.StoppingDistance = 0.2f;

	// 推进 1 秒，移动约 5m
	NavMeshSystem::Update(scene, 1.0f, nav);
	assert(xform.Location.x > 6.0f);
	assert(agent.ReachedDestination == 0);

	// 再推进 1 秒，到达目的地
	NavMeshSystem::Update(scene, 1.0f, nav);
	assert(std::abs(xform.Location.x - 10.0f) <= agent.StoppingDistance + 0.1f);
	assert(agent.ReachedDestination == 1);

	std::cout << "[PASS] TestNavMeshAgentMovementAndArrival\n";
}


static void TestNavMeshGenericTrianglesBaking()
{
	NavMesh nav;
	std::vector<NavMeshTriangle> tris;

	// Ground plane: 2 CCW triangles (0,0) to (10,10) at Y=0
	tris.push_back({ {0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 10.0f}, {10.0f, 0.0f, 0.0f} });
	tris.push_back({ {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 10.0f}, {10.0f, 0.0f, 10.0f} });

	// Raised platform: 2 CCW triangles (10,0) to (20,10) at Y=1.0
	tris.push_back({ {10.0f, 1.0f, 0.0f}, {20.0f, 1.0f, 10.0f}, {20.0f, 1.0f, 0.0f} });
	tris.push_back({ {10.0f, 1.0f, 0.0f}, {10.0f, 1.0f, 10.0f}, {20.0f, 1.0f, 10.0f} });

	NavMeshBakeSettings settings;
	settings.VoxelSize = 2.0f;
	settings.StepHeight = 1.2f;
	nav.BakeFromTriangles(tris, settings);

	assert(nav.GetPolygonCount() > 0);
	assert(nav.GetVertexCount() > 0);

	// Test path across height step
	std::vector<glm::vec3> path;
	const bool found = nav.FindPath(glm::vec3(2.0f, 0.0f, 5.0f), glm::vec3(18.0f, 1.0f, 5.0f), path);
	assert(found);
	assert(path.size() >= 2);

	std::cout << "[PASS] TestNavMeshGenericTrianglesBaking\n";
}

static void TestNavMeshAreaCostsAndPathfinding()
{
	NavMesh nav;
	// Two parallel paths:
	// Path A (Direct through Mud, Cost = 3.0)
	// Path B (Detour on Road, Cost = 0.5)
	const uint32_t v0 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 0.0f));
	const uint32_t v1 = nav.AddVertex(glm::vec3(10.0f, 0.0f, 0.0f));
	const uint32_t v2 = nav.AddVertex(glm::vec3(10.0f, 0.0f, 4.0f));
	const uint32_t v3 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 4.0f));
	const uint32_t pMud = nav.AddPolygon({ v0, v1, v2, v3 });

	const uint32_t v4 = nav.AddVertex(glm::vec3(10.0f, 0.0f, 8.0f));
	const uint32_t v5 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 8.0f));
	const uint32_t pRoad = nav.AddPolygon({ v3, v2, v4, v5 });

	nav.BuildAdjacency();

	nav.GetPolygonMut(pMud).AreaType = 2; // Mud
	nav.GetPolygonMut(pMud).Cost = 3.0f;

	nav.GetPolygonMut(pRoad).AreaType = 1; // Road
	nav.GetPolygonMut(pRoad).Cost = 0.5f;

	std::vector<glm::vec3> path;
	const bool ok = nav.FindPath(glm::vec3(1.0f, 0.0f, 2.0f), glm::vec3(9.0f, 0.0f, 6.0f), path);
	assert(ok);
	assert(path.size() >= 2);

	std::cout << "[PASS] TestNavMeshAreaCostsAndPathfinding\n";
}

static void TestNavMeshRaycastLineOfSight()
{
	NavMesh nav;
	const uint32_t v0 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 0.0f));
	const uint32_t v1 = nav.AddVertex(glm::vec3(10.0f, 0.0f, 0.0f));
	const uint32_t v2 = nav.AddVertex(glm::vec3(10.0f, 0.0f, 10.0f));
	const uint32_t v3 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 10.0f));
	const uint32_t p0 = nav.AddPolygon({ v0, v1, v2, v3 });
	(void)p0;
	nav.BuildAdjacency();

	glm::vec3 hitPoint { 0.0f };
	// LoS within polygon bounds
	const bool clear = nav.Raycast(glm::vec3(2.0f, 0.0f, 2.0f), glm::vec3(8.0f, 0.0f, 8.0f), &hitPoint);
	assert(clear);

	// LoS crossing out of polygon bounds
	const bool blocked = nav.Raycast(glm::vec3(5.0f, 0.0f, 5.0f), glm::vec3(25.0f, 0.0f, 5.0f), &hitPoint);
	assert(!blocked);

	std::cout << "[PASS] TestNavMeshRaycastLineOfSight\n";
}

static void TestNavMeshRandomWanderSampling()
{
	NavMesh nav;
	const uint32_t v0 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 0.0f));
	const uint32_t v1 = nav.AddVertex(glm::vec3(20.0f, 0.0f, 0.0f));
	const uint32_t v2 = nav.AddVertex(glm::vec3(20.0f, 0.0f, 20.0f));
	const uint32_t v3 = nav.AddVertex(glm::vec3(0.0f, 0.0f, 20.0f));
	nav.AddPolygon({ v0, v1, v2, v3 });
	nav.BuildAdjacency();

	glm::vec3 sample { 0.0f };
	const bool ok = nav.GetRandomPointAround(glm::vec3(10.0f, 0.0f, 10.0f), 5.0f, sample);
	assert(ok);
	assert(sample.x >= 0.0f && sample.x <= 20.0f);
	assert(sample.z >= 0.0f && sample.z <= 20.0f);

	std::cout << "[PASS] TestNavMeshRandomWanderSampling\n";
}

static void TestNavMeshBakeFromScene()
{
	WorldContext context;
	Scene scene(context);

	const entt::entity ground = scene.GetRegistry().create();
	auto& gt = scene.GetRegistry().emplace<TransformComponent>(ground);
	gt.Location = glm::vec3(10.0f, 0.0f, 10.0f);
	auto& box = scene.GetRegistry().emplace<BoxCollider3DComponent>(ground);
	box.HalfExtents = glm::vec3(10.0f, 0.5f, 10.0f);

	const entt::entity linkEnt = scene.GetRegistry().create();
	auto& lt = scene.GetRegistry().emplace<TransformComponent>(linkEnt);
	lt.Location = glm::vec3(5.0f, 0.5f, 5.0f);
	auto& linkComp = scene.GetRegistry().emplace<NavMeshOffMeshLinkComponent>(linkEnt);
	linkComp.TargetLocation = glm::vec3(15.0f, 0.5f, 15.0f);

	const entt::entity areaEnt = scene.GetRegistry().create();
	auto& at = scene.GetRegistry().emplace<TransformComponent>(areaEnt);
	at.Location = glm::vec3(10.0f, 0.5f, 10.0f);
	auto& areaComp = scene.GetRegistry().emplace<NavMeshAreaModifierComponent>(areaEnt);
	areaComp.Extents = glm::vec3(2.0f, 1.0f, 2.0f);
	areaComp.AreaType = 1;
	areaComp.CostMultiplier = 0.5f;

	NavMesh nav;
	NavMeshBakeSettings settings;
	settings.VoxelSize = 2.0f;
	settings.IncludeColliders = true;
	nav.BakeFromScene(scene, settings);

	assert(nav.GetPolygonCount() > 0);
	assert(nav.GetVertexCount() > 0);
	assert(nav.GetOffMeshLinks().size() == 1);
	assert(nav.GetAreaModifiers().size() == 1);

	std::vector<glm::vec3> path;
	const bool found = nav.FindPath(glm::vec3(2.0f, 0.5f, 2.0f), glm::vec3(18.0f, 0.5f, 18.0f), path);
	assert(found);
	assert(path.size() >= 2);

	std::cout << "[PASS] TestNavMeshBakeFromScene\n";
}

static void TestNavMeshAsyncBaking()
{
	WorldContext context;
	Scene scene(context);

	const entt::entity ground = scene.GetRegistry().create();
	auto& gt = scene.GetRegistry().emplace<TransformComponent>(ground);
	gt.Location = glm::vec3(10.0f, 0.0f, 10.0f);
	auto& box = scene.GetRegistry().emplace<BoxCollider3DComponent>(ground);
	box.HalfExtents = glm::vec3(10.0f, 0.5f, 10.0f);

	NavMeshBakeSettings settings;
	settings.VoxelSize = 2.0f;
	settings.IncludeColliders = true;

	auto fut = NavMesh::BakeSceneAsync(scene, settings);
	assert(fut.valid());
	fut.wait();
	auto nav = fut.get();
	assert(nav != nullptr);
	assert(nav->GetPolygonCount() > 0);

	std::cout << "[PASS] TestNavMeshAsyncBaking\n";
}

static void TestNavMeshMacroTilesLOD()
{
	NavMesh nav;

	// 1. 空网格构建宏观瓦片验证
	nav.BuildMacroTiles(20.0f);
	assert(nav.GetMacroTiles().empty());

	// 2. 构造跨两个宏观网格区域的多边形:
	// 网格大小 tileSize = 20.0f
	// Tile A: [0, 20] x [0, 20] (gx=0, gz=0), 包含两个多边形 (高度 Y=2.0f)
	// Tile B: [20, 40] x [0, 20] (gx=1, gz=0), 包含一个多边形 (高度 Y=5.0f, AreaType=Road)
	const uint32_t v0 = nav.AddVertex(glm::vec3(0.0f, 2.0f, 0.0f));
	const uint32_t v1 = nav.AddVertex(glm::vec3(10.0f, 2.0f, 0.0f));
	const uint32_t v2 = nav.AddVertex(glm::vec3(10.0f, 2.0f, 10.0f));
	const uint32_t v3 = nav.AddVertex(glm::vec3(0.0f, 2.0f, 10.0f));

	const uint32_t v4 = nav.AddVertex(glm::vec3(10.0f, 2.0f, 0.0f));
	const uint32_t v5 = nav.AddVertex(glm::vec3(18.0f, 2.0f, 0.0f));
	const uint32_t v6 = nav.AddVertex(glm::vec3(18.0f, 2.0f, 10.0f));
	const uint32_t v7 = nav.AddVertex(glm::vec3(10.0f, 2.0f, 10.0f));

	nav.AddPolygon({ v0, v1, v2, v3 });
	nav.AddPolygon({ v4, v5, v6, v7 });

	// Tile B 内的多边形
	const uint32_t v8 = nav.AddVertex(glm::vec3(22.0f, 5.0f, 2.0f));
	const uint32_t v9 = nav.AddVertex(glm::vec3(38.0f, 5.0f, 2.0f));
	const uint32_t v10 = nav.AddVertex(glm::vec3(38.0f, 5.0f, 18.0f));
	const uint32_t v11 = nav.AddVertex(glm::vec3(22.0f, 5.0f, 18.0f));

	const uint32_t p2 = nav.AddPolygon({ v8, v9, v10, v11 });
	nav.GetPolygonMut(p2).AreaType = 1; // Road

	nav.BuildMacroTiles(20.0f);
	const auto& macroTiles = nav.GetMacroTiles();
	assert(macroTiles.size() == 2);

	// 校验 Tile 0 (gx=0, gz=0)
	const auto& tile0 = macroTiles[0];
	assert(tile0.PolyCount == 2);
	assert(tile0.DominantAreaType == 0);
	assert(!tile0.Blocked);
	assert(std::abs(tile0.MinY - 2.0f) < 1e-4f);
	assert(std::abs(tile0.MaxY - 2.0f) < 1e-4f);
	assert(std::abs(tile0.Center.x - 10.0f) < 1e-4f);
	assert(std::abs(tile0.Center.y - 2.0f) < 1e-4f);
	assert(std::abs(tile0.Center.z - 10.0f) < 1e-4f);
	// 4 个外角坐标
	assert(std::abs(tile0.Corners[0].x - 0.0f) < 1e-4f && std::abs(tile0.Corners[0].z - 0.0f) < 1e-4f);
	assert(std::abs(tile0.Corners[1].x - 20.0f) < 1e-4f && std::abs(tile0.Corners[1].z - 0.0f) < 1e-4f);
	assert(std::abs(tile0.Corners[2].x - 20.0f) < 1e-4f && std::abs(tile0.Corners[2].z - 20.0f) < 1e-4f);
	assert(std::abs(tile0.Corners[3].x - 0.0f) < 1e-4f && std::abs(tile0.Corners[3].z - 20.0f) < 1e-4f);
	for (int i = 0; i < 4; ++i)
		assert(std::abs(tile0.Corners[i].y - 2.0f) < 1e-4f);

	// 校验 Tile 1 (gx=1, gz=0)
	const auto& tile1 = macroTiles[1];
	assert(tile1.PolyCount == 1);
	assert(tile1.DominantAreaType == 1);
	assert(!tile1.Blocked);
	assert(std::abs(tile1.MinY - 5.0f) < 1e-4f);
	assert(std::abs(tile1.MaxY - 5.0f) < 1e-4f);
	assert(std::abs(tile1.Center.x - 30.0f) < 1e-4f);
	assert(std::abs(tile1.Center.y - 5.0f) < 1e-4f);
	assert(std::abs(tile1.Center.z - 10.0f) < 1e-4f);
	assert(std::abs(tile1.Corners[0].x - 20.0f) < 1e-4f && std::abs(tile1.Corners[0].z - 0.0f) < 1e-4f);
	assert(std::abs(tile1.Corners[1].x - 40.0f) < 1e-4f && std::abs(tile1.Corners[1].z - 0.0f) < 1e-4f);
	assert(std::abs(tile1.Corners[2].x - 40.0f) < 1e-4f && std::abs(tile1.Corners[2].z - 20.0f) < 1e-4f);
	assert(std::abs(tile1.Corners[3].x - 20.0f) < 1e-4f && std::abs(tile1.Corners[3].z - 20.0f) < 1e-4f);

	// 3. 动态阻挡触发与自动重建
	nav.ApplyObstacle(glm::vec3(30.0f, 5.0f, 10.0f), glm::vec3(10.0f, 2.0f, 10.0f), true);
	assert(nav.GetMacroTiles().size() == 2);
	assert(nav.GetMacroTiles()[1].Blocked);
	assert(!nav.GetMacroTiles()[0].Blocked);

	// 清除阻挡
	nav.ClearObstacles();
	assert(nav.GetMacroTiles().size() == 2);
	assert(!nav.GetMacroTiles()[1].Blocked);

	// 4. 三角形烘焙自动构建宏观瓦片验证
	NavMeshTriangle tri1 { {0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 10.0f}, {10.0f, 0.0f, 0.0f} };
	NavMeshTriangle tri2 { {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 10.0f}, {10.0f, 0.0f, 10.0f} };
	nav.BakeFromTriangles({ tri1, tri2 });
	assert(!nav.GetMacroTiles().empty());
	assert(nav.GetMacroTiles()[0].PolyCount > 0);

	std::cout << "[PASS] TestNavMeshMacroTilesLOD\n";
}

static void TestNavMeshLowCeilingAndAgentHeightFiltering()
{
	NavMesh nav;
	// 建立 3 个连续四边形通道: (0,0) -> (10,0) -> (20,0) -> (30,0)
	uint32_t v[8];
	v[0] = nav.AddVertex(glm::vec3(0.0f, 0.0f, 0.0f));
	v[1] = nav.AddVertex(glm::vec3(10.0f, 0.0f, 0.0f));
	v[2] = nav.AddVertex(glm::vec3(20.0f, 0.0f, 0.0f));
	v[3] = nav.AddVertex(glm::vec3(30.0f, 0.0f, 0.0f));

	v[4] = nav.AddVertex(glm::vec3(0.0f, 0.0f, 10.0f));
	v[5] = nav.AddVertex(glm::vec3(10.0f, 0.0f, 10.0f));
	v[6] = nav.AddVertex(glm::vec3(20.0f, 0.0f, 10.0f));
	v[7] = nav.AddVertex(glm::vec3(30.0f, 0.0f, 10.0f));

	nav.AddPolygon({ v[0], v[1], v[5], v[4] }); // 0
	uint32_t pMid = nav.AddPolygon({ v[1], v[2], v[6], v[5] }); // 1 (中间限高半高通道)
	nav.AddPolygon({ v[2], v[3], v[7], v[6] }); // 2
	nav.BuildAdjacency();

	// 将中间多边形配置为半高通道 (LowCeiling, 净空 1.2m)
	nav.GetPolygonMut(pMid).AreaType = NavAreaType::LowCeiling;
	nav.GetPolygonMut(pMid).Clearance = 1.2f;

	const glm::vec3 startPt(2.0f, 0.0f, 5.0f);
	const glm::vec3 endPt(28.0f, 0.0f, 5.0f);

	// 1. 高角色 (Tall Agent: 身高 1.8m > 1.2m 净空) 站立态寻路 —— 无法通过半高通道！
	{
		NavQueryFilter tallFilter;
		tallFilter.AgentHeight = 1.8f;
		tallFilter.RequireClearance = true;
		std::vector<glm::vec3> path;
		bool found = nav.FindPath(startPt, endPt, path, tallFilter);
		assert(!found); // 高角色无法穿过半高矮通道
	}

	// 2. 矮小角色 / 爬虫 (Short Agent: 身高 1.0m <= 1.2m 净空) —— 顺畅通过！
	{
		NavQueryFilter shortFilter;
		shortFilter.AgentHeight = 1.0f;
		shortFilter.RequireClearance = true;
		std::vector<glm::vec3> path;
		bool found = nav.FindPath(startPt, endPt, path, shortFilter);
		assert(found);
		assert(path.size() >= 2);
	}

	// 3. 高角色进入蹲伏潜行态 (CrouchHeight = 0.9m <= 1.2m 净空) —— 允许钻入并顺畅通过！
	{
		NavMeshAgentComponent agent;
		agent.AgentHeight = 1.8f;
		agent.CrouchHeight = 0.9f;
		agent.Crouching = 1;

		NavQueryFilter crouchFilter;
		crouchFilter.AgentHeight = agent.GetEffectiveHeight();
		crouchFilter.RequireClearance = true;
		std::vector<glm::vec3> path;
		bool found = nav.FindPath(startPt, endPt, path, crouchFilter);
		assert(found);
		assert(path.size() >= 2);
	}

	// 4. 掩码明确禁用 LowCeiling 区域 (AreaMask 过滤) —— 阻断无法通行
	{
		NavQueryFilter maskBlockedFilter;
		maskBlockedFilter.AgentHeight = 1.0f;
		maskBlockedFilter.AreaMask = ~(1u << NavAreaType::LowCeiling); // 显式禁用 LowCeiling
		std::vector<glm::vec3> path;
		bool found = nav.FindPath(startPt, endPt, path, maskBlockedFilter);
		assert(!found);
	}

	std::cout << "[PASS] TestNavMeshLowCeilingAndAgentHeightFiltering\n";
}


static void TestNavMeshMultiLayeredOverlappingSurfaces()
{
	NavMesh nav;
	std::vector<NavMeshTriangle> tris;

	// 构造 3D 立体多层几何体：
	// 1. 下层地面通道 (Ground Floor, Y = 0.0f):
	//    覆盖 X in [0.0, 30.0], Z in [0.0, 10.0]，分为三段 (西引道 0-10, 涵洞区 10-20, 东引道 20-30)
	auto addQuadTris = [&](float x0, float z0, float x1, float z1, float y) {
		// Tri 1 (CCW with upward normal)
		tris.push_back({ {x0, y, z0}, {x1, y, z1}, {x1, y, z0} });
		// Tri 2 (CCW with upward normal)
		tris.push_back({ {x0, y, z0}, {x0, y, z1}, {x1, y, z1} });
	};

	// 下层地面 3 段
	addQuadTris(0.0f, 0.0f, 10.0f, 10.0f, 0.0f);
	addQuadTris(10.0f, 0.0f, 20.0f, 10.0f, 0.0f);
	addQuadTris(20.0f, 0.0f, 30.0f, 10.0f, 0.0f);

	// 2. 上层立体大桥 (Overhead Bridge, Y = 4.0f):
	//    横跨中间区域 X in [10.0, 20.0], Z in [0.0, 10.0]，与下层涵洞形成严格物理投影重叠
	addQuadTris(10.0f, 0.0f, 20.0f, 10.0f, 4.0f);

	NavMeshBakeSettings settings;
	settings.VoxelSize = 2.0f;
	settings.StepHeight = 0.5f;
	nav.BakeFromTriangles(tris, settings);

	assert(nav.GetPolygonCount() > 0);

	// 验证 1: 投影重叠区域内 (X in [10, 20], Z in [0, 10])，上层大桥多边形与下层多边形独立并存
	// 且下层多边形自动推导获得精确垂直物理净空 Clearance = 4.0f, 上层大桥为露天开放净空 (Clearance = 0.0f)
	uint32_t overlapLowerCount = 0;
	uint32_t overlapUpperCount = 0;
	for (size_t i = 0; i < nav.GetPolygonCount(); ++i)
	{
		const auto& poly = nav.GetPolygon(static_cast<uint32_t>(i));
		if (poly.Center.x >= 10.0f - 1e-3f && poly.Center.x <= 20.0f + 1e-3f)
		{
			if (std::abs(poly.Center.y - 0.0f) < 0.2f)
			{
				overlapLowerCount++;
				assert(std::abs(poly.Clearance - 4.0f) < 1e-3f);
				assert(std::abs(poly.BaseClearance - 4.0f) < 1e-3f);
			}
			else if (std::abs(poly.Center.y - 4.0f) < 0.2f)
			{
				overlapUpperCount++;
				assert(std::abs(poly.Clearance - 0.0f) < 1e-3f);
				assert(std::abs(poly.BaseClearance - 0.0f) < 1e-3f);
			}
		}
	}
	assert(overlapLowerCount > 0);
	assert(overlapUpperCount > 0);
	assert(overlapLowerCount == overlapUpperCount);

	// 验证 2: 空间采样过滤 —— 在桥底涵洞中心采样
	// 短代理 (身高 1.0m <= 4.0m 净空) 可以成功立足下层
	NavQueryFilter shortFilter;
	shortFilter.AgentHeight = 1.0f;
	shortFilter.RequireClearance = true;

	glm::vec3 sampledPt;
	uint32_t sampledPoly = 0;
	bool canSampleShort = nav.SamplePosition(glm::vec3(15.0f, 0.0f, 5.0f), 1.0f, sampledPt, sampledPoly, shortFilter);
	assert(canSampleShort);
	assert(std::abs(nav.GetPolygon(sampledPoly).Clearance - 4.0f) < 1e-3f);

	// 超高代理 (身高 5.0m > 4.0m 净空) 在桥底局部采样被垂直净空物理拦截
	NavQueryFilter tallFilter;
	tallFilter.AgentHeight = 5.0f;
	tallFilter.RequireClearance = true;
	bool canSampleTallLower = nav.SamplePosition(glm::vec3(15.0f, 0.0f, 5.0f), 1.0f, sampledPt, sampledPoly, tallFilter);
	assert(!canSampleTallLower);

	// 验证 3: 下层端到端寻路 (从西引道 X=2.0 穿越桥底到东引道 X=28.0)
	// 短代理畅通无阻穿越桥底涵洞
	std::vector<glm::vec3> lowerShortPath;
	bool foundShort = nav.FindPath(glm::vec3(2.0f, 0.0f, 5.0f), glm::vec3(28.0f, 0.0f, 5.0f), lowerShortPath, shortFilter);
	assert(foundShort);
	assert(lowerShortPath.size() >= 2);
	assert(lowerShortPath.front().x <= 2.1f);
	assert(lowerShortPath.back().x >= 27.9f);

	// 超高代理受 4.0m 桥底垂直净空物理阻断，无法从下层涵洞穿行
	std::vector<glm::vec3> lowerTallPath;
	bool foundTallLower = nav.FindPath(glm::vec3(2.0f, 0.0f, 5.0f), glm::vec3(28.0f, 0.0f, 5.0f), lowerTallPath, tallFilter);
	assert(!foundTallLower);

	// 验证 4: 上层高架大桥寻路 (从大桥西端 X=12.0 到大桥东端 X=18.0)
	// 超高代理与短代理在大桥桥面均可顺畅通行
	std::vector<glm::vec3> upperTallPath;
	bool foundTallUpper = nav.FindPath(glm::vec3(12.0f, 4.0f, 5.0f), glm::vec3(18.0f, 4.0f, 5.0f), upperTallPath, tallFilter);
	assert(foundTallUpper);
	assert(upperTallPath.size() >= 2);
	assert(upperTallPath.front().x <= 12.1f);
	assert(upperTallPath.back().x >= 17.9f);

	std::cout << "[PASS] TestNavMeshMultiLayeredOverlappingSurfaces (Overlapped Quads: " << overlapLowerCount << "x2)\n";
}

static void TestNavMeshAreaModifierCenterYOffsetClearance()
{
	NavMesh nav;
	// 建立 3 个连续四边形通道: (0,0) -> (10,0) -> (20,0) -> (30,0)
	uint32_t v[8];
	v[0] = nav.AddVertex(glm::vec3(0.0f, 0.0f, 0.0f));
	v[1] = nav.AddVertex(glm::vec3(10.0f, 0.0f, 0.0f));
	v[2] = nav.AddVertex(glm::vec3(20.0f, 0.0f, 0.0f));
	v[3] = nav.AddVertex(glm::vec3(30.0f, 0.0f, 0.0f));

	v[4] = nav.AddVertex(glm::vec3(0.0f, 0.0f, 10.0f));
	v[5] = nav.AddVertex(glm::vec3(10.0f, 0.0f, 10.0f));
	v[6] = nav.AddVertex(glm::vec3(20.0f, 0.0f, 10.0f));
	v[7] = nav.AddVertex(glm::vec3(30.0f, 0.0f, 10.0f));

	nav.AddPolygon({ v[0], v[1], v[5], v[4] }); // 0
	uint32_t pMid = nav.AddPolygon({ v[1], v[2], v[6], v[5] }); // 1 (中间通道，Center 为 (15.0, 0.0, 5.0))
	nav.AddPolygon({ v[2], v[3], v[7], v[6] }); // 2
	nav.BuildAdjacency();

	// 添加带有 Y 轴位移 (Center.y = 0.6f > 0.05f) 的 AreaModifier，配置 ClearanceHeight = 1.3f
	NavMeshAreaModifier mod;
	mod.Center = glm::vec3(15.0f, 0.6f, 5.0f);
	mod.Extents = glm::vec3(5.0f, 2.0f, 5.0f);
	mod.AreaType = NavAreaType::LowCeiling;
	mod.CostMultiplier = 1.0f;
	mod.ClearanceHeight = 1.3f;
	nav.AddAreaModifier(mod);

	// 1. 验证受影响的多边形 Clearance 严格等于 1.3f，绝不膨胀为 1.9f (1.3 + 0.6)
	const auto& midPoly = nav.GetPolygon(pMid);
	assert(std::abs(midPoly.Clearance - 1.3f) < 1e-4f);

	const glm::vec3 startPt(2.0f, 0.0f, 5.0f);
	const glm::vec3 endPt(28.0f, 0.0f, 5.0f);

	// 2. 验证身高 1.8m 的 tallFilter (站立人偶) 寻路失败 (受 1.3m 限高拦截)
	{
		NavQueryFilter tallFilter;
		tallFilter.AgentHeight = 1.8f;
		tallFilter.RequireClearance = true;
		std::vector<glm::vec3> path;
		bool found = nav.FindPath(startPt, endPt, path, tallFilter);
		assert(!found);
	}

	// 3. 验证身高 1.0m 的 shortFilter 顺畅寻路成功
	{
		NavQueryFilter shortFilter;
		shortFilter.AgentHeight = 1.0f;
		shortFilter.RequireClearance = true;
		std::vector<glm::vec3> path;
		bool found = nav.FindPath(startPt, endPt, path, shortFilter);
		assert(found);
		assert(path.size() >= 2);
	}

	std::cout << "[PASS] TestNavMeshAreaModifierCenterYOffsetClearance\n";
}

static void TestNavMeshOffMeshLinkCostAndChokepoint()
{
	NavMesh nav;

	// 构建 3 个连续平地四边形通道:
	// Poly 0: [0, 10] x [0, 10], Center (5, 0, 5)
	// Poly 1: [10, 20] x [0, 10], Center (15, 0, 5)
	// Poly 2: [20, 30] x [0, 10], Center (25, 0, 5)
	uint32_t v[8];
	v[0] = nav.AddVertex(glm::vec3(0.0f, 0.0f, 0.0f));
	v[1] = nav.AddVertex(glm::vec3(10.0f, 0.0f, 0.0f));
	v[2] = nav.AddVertex(glm::vec3(20.0f, 0.0f, 0.0f));
	v[3] = nav.AddVertex(glm::vec3(30.0f, 0.0f, 0.0f));

	v[4] = nav.AddVertex(glm::vec3(0.0f, 0.0f, 10.0f));
	v[5] = nav.AddVertex(glm::vec3(10.0f, 0.0f, 10.0f));
	v[6] = nav.AddVertex(glm::vec3(20.0f, 0.0f, 10.0f));
	v[7] = nav.AddVertex(glm::vec3(30.0f, 0.0f, 10.0f));

	uint32_t p0 = nav.AddPolygon({ v[0], v[1], v[5], v[4] }); // 0
	uint32_t p1 = nav.AddPolygon({ v[1], v[2], v[6], v[5] }); // 1
	uint32_t p2 = nav.AddPolygon({ v[2], v[3], v[7], v[6] }); // 2

	// 在侧方构建一个独立的平台多边形 3: [10, 20] x [20, 30], Center (15, 0, 25)
	uint32_t vPlatform[4];
	vPlatform[0] = nav.AddVertex(glm::vec3(10.0f, 0.0f, 20.0f));
	vPlatform[1] = nav.AddVertex(glm::vec3(20.0f, 0.0f, 20.0f));
	vPlatform[2] = nav.AddVertex(glm::vec3(20.0f, 0.0f, 30.0f));
	vPlatform[3] = nav.AddVertex(glm::vec3(10.0f, 0.0f, 30.0f));
	uint32_t pPlatform = nav.AddPolygon({ vPlatform[0], vPlatform[1], vPlatform[2], vPlatform[3] }); // 3

	nav.BuildAdjacency();

	// 验证 HasSharedBoundary 正确反映几何连通性
	assert(nav.HasSharedBoundary(p0, p1));
	assert(nav.HasSharedBoundary(p1, p2));
	assert(!nav.HasSharedBoundary(p0, p2));
	assert(!nav.HasSharedBoundary(p1, pPlatform));

	// 在 p0 和侧方孤立平台 pPlatform 之间建立跳跃链接 Link1，在 pPlatform 和 p2 之间建立 Link2
	NavMeshOffMeshLink linkSide1;
	linkSide1.Start = glm::vec3(5.0f, 0.0f, 8.0f);
	linkSide1.End = glm::vec3(15.0f, 0.0f, 22.0f);
	linkSide1.Cost = 2.0f;
	linkSide1.BiDirectional = false;
	nav.AddOffMeshLink(linkSide1);

	NavMeshOffMeshLink linkSide2;
	linkSide2.Start = glm::vec3(15.0f, 0.0f, 28.0f);
	linkSide2.End = glm::vec3(25.0f, 0.0f, 8.0f);
	linkSide2.Cost = 2.0f;
	linkSide2.BiDirectional = false;
	nav.AddOffMeshLink(linkSide2);

	const glm::vec3 startPt(2.0f, 0.0f, 5.0f);
	const glm::vec3 endPt(28.0f, 0.0f, 5.0f);

	// 1. 验证在平地通道完整时，A* 启发式与跳跃边代价机制会严格惩罚跳跃，优先走平坦直路，绝不偏航跳跃
	{
		std::vector<glm::vec3> path;
		bool found = nav.FindPath(startPt, endPt, path);
		assert(found);
		// 平地直达只有 2 个点 (起点与终点直线贯通)
		assert(path.size() == 2);
		assert(std::abs(path[0].x - startPt.x) < 1e-3f && std::abs(path[0].z - startPt.z) < 1e-3f);
		assert(std::abs(path.back().x - endPt.x) < 1e-3f && std::abs(path.back().z - endPt.z) < 1e-3f);
		// 确保路径上没有任何路点进入侧方跳跃平台 (Z 坐标远小于 20.0f)
		for (const auto& pt : path)
		{
			assert(pt.z < 15.0f);
		}
	}

	// 2. 验证当平坦直路发生断裂/阻挡 (p1 阻塞) 时，跳跃通道成为唯一通路，能成功规划出跨跳跃点路径
	{
		nav.GetPolygonMut(p1).Blocked = true;
		std::vector<glm::vec3> path;
		bool found = nav.FindPath(startPt, endPt, path);
		assert(found);
		// 此时必须走跳跃点绕过阻塞的 p1 通道
		bool visitedPlatform = false;
		for (const auto& pt : path)
		{
			if (pt.z >= 20.0f)
			{
				visitedPlatform = true;
				break;
			}
		}
		assert(visitedPlatform);
		nav.GetPolygonMut(p1).Blocked = false; // 恢复
	}

	// 3. 验证单向跳跃限制 (BiDirectional = false) 拦截逆向跨越
	{
		nav.GetPolygonMut(p1).Blocked = true;
		std::vector<glm::vec3> reversePath;
		bool reverseFound = nav.FindPath(endPt, startPt, reversePath);
		assert(!reverseFound);
		nav.GetPolygonMut(p1).Blocked = false;
	}

	std::cout << "[PASS] TestNavMeshOffMeshLinkCostAndChokepoint\n";
}

int main()
{
	std::cout << "Starting World.NavMesh tests...\n";
	TestNavMeshPolygonBuildingAndAdjacency();
	TestNavMeshSamplePosition();
	TestNavMeshFindPathAndFunnel();
	TestNavMeshDynamicObstacleCarving();
	TestNavMeshTerrainBaking();
	TestNavMeshAgentMovementAndArrival();
	TestNavMeshGenericTrianglesBaking();
	TestNavMeshAreaCostsAndPathfinding();
	TestNavMeshRaycastLineOfSight();
	TestNavMeshRandomWanderSampling();
	TestNavMeshBakeFromScene();
	TestNavMeshAsyncBaking();
	TestNavMeshMacroTilesLOD();
	TestNavMeshLowCeilingAndAgentHeightFiltering();
	TestNavMeshMultiLayeredOverlappingSurfaces();
	TestNavMeshAreaModifierCenterYOffsetClearance();
	TestNavMeshOffMeshLinkCostAndChokepoint();
	std::cout << "All 17 World.NavMesh tests passed successfully!\n";
	return 0;
}
