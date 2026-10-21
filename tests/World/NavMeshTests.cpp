#include "World/Core/WorldContext.h"
#include "World/NavMesh/NavMesh.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Components.h"

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

int main()
{
	std::cout << "Starting World.NavMesh tests...\n";
	TestNavMeshPolygonBuildingAndAdjacency();
	TestNavMeshSamplePosition();
	TestNavMeshFindPathAndFunnel();
	TestNavMeshDynamicObstacleCarving();
	TestNavMeshTerrainBaking();
	TestNavMeshAgentMovementAndArrival();
	std::cout << "All World.NavMesh tests passed successfully!\n";
	return 0;
}

