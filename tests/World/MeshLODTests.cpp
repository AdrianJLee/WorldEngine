#include "World/Renderer/MeshLOD.h"
#include "World/Scene/SceneOctree.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <iostream>

using namespace World;

static void TestMeshLODEvaluationAndHysteresis()
{
	const float lod1Dist = 30.0f;
	const float lod2Dist = 80.0f;
	const float cullDist = 180.0f;
	const float h = 2.0f;

	// 1. 近距离 LOD0
	uint8_t lod = MeshLODEvaluator::EvaluateLOD(10.0f, lod1Dist, lod2Dist, cullDist, 0, h);
	assert(lod == 0);

	// 2. 刚好进入临界点 31m (因 h=2m，尚未超过 30+2=32m，滞后保持 LOD0)
	lod = MeshLODEvaluator::EvaluateLOD(31.0f, lod1Dist, lod2Dist, cullDist, 0, h);
	assert(lod == 0);

	// 3. 超过 33m，顺利切换到 LOD1
	lod = MeshLODEvaluator::EvaluateLOD(33.0f, lod1Dist, lod2Dist, cullDist, 0, h);
	assert(lod == 1);

	// 4. 从远往近退回 29m (因 h=2m，尚未低于 30-2=28m，滞后保持 LOD1)
	lod = MeshLODEvaluator::EvaluateLOD(29.0f, lod1Dist, lod2Dist, cullDist, 1, h);
	assert(lod == 1);

	// 5. 退回 25m，切回 LOD0
	lod = MeshLODEvaluator::EvaluateLOD(25.0f, lod1Dist, lod2Dist, cullDist, 1, h);
	assert(lod == 0);

	// 6. 远距离超过 85m，切换到 LOD2
	lod = MeshLODEvaluator::EvaluateLOD(90.0f, lod1Dist, lod2Dist, cullDist, 1, h);
	assert(lod == 2);

	// 7. 超出 180m，触发剔除
	lod = MeshLODEvaluator::EvaluateLOD(200.0f, lod1Dist, lod2Dist, cullDist, 2, h);
	assert(lod == MeshLODEvaluator::kCulled);

	std::cout << "[PASS] TestMeshLODEvaluationAndHysteresis\n";
}

static void TestSceneOctreeBuildAndQuery()
{
	SceneOctree octree;

	std::vector<OctreeElement> elements;
	// 生成 64 个分散在 [-100, 100]³ 空间内的实体
	for (int z = -2; z <= 2; ++z)
	{
		for (int y = -2; y <= 2; ++y)
		{
			for (int x = -2; x <= 2; ++x)
			{
				OctreeElement elem;
				elem.EntityId = static_cast<uint32_t>(elements.size() + 1);
				glm::vec3 pos(x * 30.0f, y * 30.0f, z * 30.0f);
				elem.BoundsMin = pos - glm::vec3(2.0f);
				elem.BoundsMax = pos + glm::vec3(2.0f);
				elements.push_back(elem);
			}
		}
	}

	octree.Build(elements, glm::vec3(0.0f), 150.0f, 4, 8);
	assert(octree.GetTotalElementCount() == elements.size());
	assert(octree.GetTotalNodeCount() > 1);

	// 1. 射线查询 (沿着 X 轴从 (-100, 0, 0) 射向 (+100, 0, 0))
	std::vector<uint32_t> rayHits;
	octree.QueryRay(glm::vec3(-100.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 200.0f, rayHits);
	assert(!rayHits.empty());
	assert(rayHits.size() < elements.size()); // 精确过滤，不返回全部元素

	// 2. 球形近邻查询 (在原点半长 20m)
	std::vector<uint32_t> sphereHits;
	octree.QuerySphere(glm::vec3(0.0f), 20.0f, sphereHits);
	assert(!sphereHits.empty());
	assert(sphereHits.size() < elements.size());

	std::cout << "[PASS] TestSceneOctreeBuildAndQuery (Nodes: "
		<< octree.GetTotalNodeCount() << ", Elements: " << octree.GetTotalElementCount() << ")\n";
}

static void TestMeshLODComponentLayout()
{
	MeshLODComponent comp;
	assert(sizeof(comp) == 64);
	assert(comp.LOD1Distance == 30.0f);
	assert(comp.LOD2Distance == 80.0f);
	assert(comp.CullDistance == 180.0f);
	assert(comp.Hysteresis == 2.0f);

	std::cout << "[PASS] TestMeshLODComponentLayout\n";
}

int main()
{
	std::cout << "Starting World.MeshLOD tests...\n";
	TestMeshLODEvaluationAndHysteresis();
	TestSceneOctreeBuildAndQuery();
	TestMeshLODComponentLayout();
	std::cout << "All World.MeshLOD tests passed successfully!\n";
	return 0;
}
