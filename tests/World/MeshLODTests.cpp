#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#include "World/Renderer/MeshLOD.h"
#include "World/Renderer/MeshLODGenerator.h"
#include "World/Scene/SceneOctree.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/WorldContext.h"
#include "World/Renderer/FrustumCull.h"

#include <glm/gtc/matrix_transform.hpp>
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
	assert(comp.LOD1Distance == 15.0f);
	assert(comp.LOD2Distance == 30.0f);
	assert(comp.LOD3Distance == 50.0f);
	assert(comp.LOD4Distance == 75.0f);
	assert(comp.LOD5Distance == 105.0f);
	assert(comp.LOD6Distance == 140.0f);
	assert(comp.LOD7Distance == 180.0f);
	assert(comp.CullDistance == 240.0f);
	assert(comp.Hysteresis == 2.0f);
	assert(comp.LODBias == 1.0f);
	assert(comp.ForcedLOD == -1);
	assert(comp.MaxLODLevel == 7);
	assert(comp.CurrentLOD == 0);
	assert(comp.EnableCrossFade == 1);

	// 8 级动态阶梯测试
	const float dists[7] = { 10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f };
	uint8_t dLOD = MeshLODEvaluator::EvaluateDynamicLOD(5.0f, dists, 7, 100.0f, 0, 1.0f);
	assert(dLOD == 0);
	dLOD = MeshLODEvaluator::EvaluateDynamicLOD(25.0f, dists, 7, 100.0f, 0, 1.0f);
	assert(dLOD == 2);
	dLOD = MeshLODEvaluator::EvaluateDynamicLOD(75.0f, dists, 7, 100.0f, 0, 1.0f);
	assert(dLOD == 7);
	dLOD = MeshLODEvaluator::EvaluateDynamicLOD(120.0f, dists, 7, 100.0f, 7, 1.0f);
	assert(dLOD == MeshLODEvaluator::kCulled);

	std::cout << "[PASS] TestMeshLODComponentLayout & Dynamic 8 Levels\n";
}

static void TestMeshLODManager()
{
	MeshLODManager& mgr = MeshLODManager::Get();
	mgr.ResetStats();
	mgr.SetForcedLOD(-1);
	mgr.SetGlobalDistanceScale(1.0f);
	mgr.SetLODColorationEnabled(false);

	assert(mgr.GetForcedLOD() == -1);
	assert(mgr.GetGlobalDistanceScale() == 1.0f);
	assert(!mgr.IsLODColorationEnabled());

	MeshLODComponent comp;
	comp.LOD1Distance = 30.0f;
	comp.LOD2Distance = 80.0f;
	comp.CullDistance = 180.0f;
	comp.Hysteresis = 2.0f;
	comp.MaxLODLevel = 2;

	// 1. Evaluate with component hysteresis
	uint8_t lod = mgr.EvaluateLOD(10.0f, comp, 0);
	assert(lod == 0);
	lod = mgr.EvaluateLOD(35.0f, comp, 0);
	assert(lod == 1);
	lod = mgr.EvaluateLOD(90.0f, comp, 1);
	assert(lod == 2);
	lod = mgr.EvaluateLOD(200.0f, comp, 2);
	assert(lod == MeshLODEvaluator::kCulled);

	// 2. 5% default hysteresis when comp.Hysteresis <= 0.0f
	comp.Hysteresis = 0.0f; // triggers 5% default
	// At 30m, 5% is 1.5m, so 30+1.5=31.5m threshold
	lod = mgr.EvaluateLOD(31.0f, comp, 0);
	assert(lod == 0); // within 5% band
	lod = mgr.EvaluateLOD(32.0f, comp, 0);
	assert(lod == 1); // exceeded 5% band

	// 3. Global distance scale
	comp.Hysteresis = 2.0f;
	mgr.SetGlobalDistanceScale(2.0f);
	assert(mgr.GetGlobalDistanceScale() == 2.0f);
	// 16m * 2.0 = 32m > 30+2=32m -> switch to LOD1
	lod = mgr.EvaluateLOD(17.0f, comp, 0);
	assert(lod == 1);
	mgr.SetGlobalDistanceScale(1.0f);

	// 4. Forced LOD override
	mgr.SetForcedLOD(2);
	assert(mgr.GetForcedLOD() == 2);
	assert(mgr.EvaluateLOD(5.0f, comp, 0) == 2);
	mgr.SetForcedLOD(0);
	assert(mgr.EvaluateLOD(500.0f, comp, 0) == 0);
	mgr.SetForcedLOD(-1); // restore Auto

	// 5. Statistics recording & reset
	mgr.ResetStats();
	assert(mgr.GetStats().TotalLODEntities == 0);
	mgr.RecordLODResult(0);
	mgr.RecordLODResult(1);
	mgr.RecordLODResult(2);
	mgr.RecordLODResult(MeshLODEvaluator::kCulled);

	const auto& stats = mgr.GetStats();
	assert(stats.TotalLODEntities == 4);
	assert(stats.LOD0Count == 1);
	assert(stats.LOD1Count == 1);
	assert(stats.LOD2Count == 1);
	assert(stats.CulledCount == 1);

	mgr.ResetStats();
	assert(mgr.GetStats().TotalLODEntities == 0);

	// 6. LOD coloration & colors
	mgr.SetLODColorationEnabled(true);
	assert(mgr.IsLODColorationEnabled());

	const glm::vec4 col0 = mgr.GetLODColor(0);
	const glm::vec4 col1 = mgr.GetLODColor(1);
	const glm::vec4 col2 = mgr.GetLODColor(2);
	const glm::vec4 colCulled = mgr.GetLODColor(MeshLODEvaluator::kCulled);

	assert(col0 == glm::vec4(0.2f, 0.9f, 0.2f, 1.0f));
	assert(col1 == glm::vec4(0.95f, 0.85f, 0.1f, 1.0f));
	assert(col2 == glm::vec4(0.95f, 0.45f, 0.1f, 1.0f));
	assert(colCulled == glm::vec4(0.8f, 0.15f, 0.15f, 1.0f));

	mgr.SetLODColorationEnabled(false);
	assert(!mgr.IsLODColorationEnabled());

	std::cout << "[PASS] TestMeshLODManager\n";
}

static void TestSceneOctreeExtensions()
{
	SceneOctree octree;

	std::vector<OctreeElement> elements;
	for (uint32_t i = 1; i <= 16; ++i)
	{
		OctreeElement elem;
		elem.EntityId = i;
		glm::vec3 pos(i * 10.0f, 0.0f, 0.0f);
		elem.BoundsMin = pos - glm::vec3(2.0f);
		elem.BoundsMax = pos + glm::vec3(2.0f);
		elements.push_back(elem);
	}

	octree.Build(elements, glm::vec3(85.0f, 0.0f, 0.0f), 100.0f, 4, 4);
	assert(octree.GetTotalElementCount() == 16);

	// 1. ForEachNode
	size_t nodeCount = 0;
	octree.ForEachNode([&](const SceneOctree::Node& node)
	{
		++nodeCount;
		assert(node.HalfSize > 0.0f);
	});
	assert(nodeCount == octree.GetTotalNodeCount());

	// 2. GetDebugBoxes
	std::vector<std::pair<glm::vec3, glm::vec3>> debugBoxes;
	octree.GetDebugBoxes(debugBoxes);
	assert(!debugBoxes.empty());
	for (const auto& box : debugBoxes)
	{
		assert(box.first.x <= box.second.x);
		assert(box.first.y <= box.second.y);
		assert(box.first.z <= box.second.z);
	}

	// 3. Remove
	bool removed = octree.Remove(5);
	assert(removed);
	assert(octree.GetTotalElementCount() == 15);

	// Removing already removed entity returns false
	assert(!octree.Remove(5));
	assert(octree.GetTotalElementCount() == 15);

	// Verify entity 5 is no longer hit by sphere query
	std::vector<uint32_t> sphereHits;
	octree.QuerySphere(glm::vec3(50.0f, 0.0f, 0.0f), 3.0f, sphereHits);
	for (uint32_t id : sphereHits)
	{
		assert(id != 5);
	}

	// 4. Update
	OctreeElement updatedElem;
	updatedElem.EntityId = 10;
	updatedElem.BoundsMin = glm::vec3(70.0f, 10.0f, 10.0f);
	updatedElem.BoundsMax = glm::vec3(74.0f, 14.0f, 14.0f);
	octree.Update(updatedElem);
	assert(octree.GetTotalElementCount() == 15); // replace, count stays 15

	sphereHits.clear();
	octree.QuerySphere(glm::vec3(72.0f, 12.0f, 12.0f), 5.0f, sphereHits);
	assert(!sphereHits.empty());
	assert(sphereHits[0] == 10);

	std::cout << "[PASS] TestSceneOctreeExtensions\n";
}

static void TestSceneOctreeIntegration()
{
	
	WorldContext context;
	Scene scene(context);

	assert(scene.GetOctree().GetTotalElementCount() == 0);
	scene.BuildOctree();
	assert(scene.GetOctree().GetTotalElementCount() == 0);

	Entity e1 = Entity::CreateEntity(&scene, "BoxActor");
	auto& tc = e1.AddComponent<TransformComponent>();
	tc.Location = glm::vec3(15.0f, 0.0f, 0.0f);
	auto& box = e1.AddComponent<BoxCollider3DComponent>();
	box.HalfExtents = glm::vec3(2.0f);

	scene.BuildOctree();
	assert(scene.GetOctree().GetTotalElementCount() == 1);

	// Frustum query containing entity 1
	glm::mat4 view = glm::lookAt(glm::vec3(15.0f, 0.0f, 50.0f), glm::vec3(15.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
	FrustumPlanes frustum = ExtractFrustumPlanes(proj * view);

	std::vector<uint32_t> visible;
	scene.GetOctree().QueryFrustum(frustum, visible);
	assert(visible.size() == 1);
	assert(visible[0] == static_cast<uint32_t>(e1));

	// Frustum query pointing away (in opposite direction)
	glm::mat4 viewAway = glm::lookAt(glm::vec3(-100.0f, 0.0f, 0.0f), glm::vec3(-200.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	FrustumPlanes frustumAway = ExtractFrustumPlanes(proj * viewAway);
	visible.clear();
	scene.GetOctree().QueryFrustum(frustumAway, visible);
	assert(visible.empty());

	std::cout << "[PASS] TestSceneOctreeIntegration\n";
}

int main()
{
#if defined(_WIN32) && defined(_DEBUG)
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
	std::cout << "Starting World.MeshLOD tests...\n";
	TestMeshLODEvaluationAndHysteresis();
	TestSceneOctreeBuildAndQuery();
	TestMeshLODComponentLayout();
	TestMeshLODManager();
	TestSceneOctreeExtensions();
	TestSceneOctreeIntegration();

	// 验证 8 级 LOD 资产批量生成与单调递减
	{
		const std::string spherePath = "Engine/content/models/primitives/sphere.wmodel";
		std::vector<LODLevelConfig> configs = {
			{ 1, 0.70f, 15.0f },
			{ 2, 0.45f, 30.0f },
			{ 3, 0.28f, 50.0f },
			{ 4, 0.16f, 75.0f },
			{ 5, 0.09f, 105.0f },
			{ 6, 0.05f, 140.0f },
			{ 7, 0.025f, 180.0f }
		};
		std::vector<GeneratedLODAsset> generated;
		std::string err;
		bool ok = MeshLODGenerator::GenerateAndSaveLODFiles(spherePath, configs, generated, &err);
		assert(ok);
		assert(generated.size() == 7);
		for (size_t i = 1; i < generated.size(); ++i)
		{
			assert(generated[i].TriangleCount <= generated[i - 1].TriangleCount);
		}
		std::cout << "[PASS] TestGenerate8LevelLODAssets (Sphere LOD1..LOD7 generated and verified)\n";
	}

	// 为角色 mannequin 与植被 pebble 生成多级 LOD 资产
	{
		const std::string mannequinPath = "Engine/content/models/characters/mannequin.wmodel";
		std::vector<LODLevelConfig> configs = {
			{ 1, 0.60f, 20.0f },
			{ 2, 0.35f, 40.0f },
			{ 3, 0.20f, 65.0f },
			{ 4, 0.12f, 95.0f },
			{ 5, 0.07f, 130.0f }
		};
		std::vector<GeneratedLODAsset> genMannequin;
		std::string err;
		MeshLODGenerator::GenerateAndSaveLODFiles(mannequinPath, configs, genMannequin, &err);

		const std::string pebblePath = "Engine/content/models/foliage/scatter_pebble.wmodel";
		std::vector<GeneratedLODAsset> genPebble;
		MeshLODGenerator::GenerateAndSaveLODFiles(pebblePath, configs, genPebble, &err);
	}
	std::cout << "All World.MeshLOD tests passed successfully!\n";
	return 0;
}




