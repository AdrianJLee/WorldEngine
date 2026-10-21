#include "World/Environment/PCGBiome.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include "World/Foliage/FoliageInteraction.h"
#include "World/Terrain/Terrain.h"
#include "World/Level/Spline3D.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestPCGBiomeClassification()
{
	assert(PCGBiomeGenerator::ClassifyBiome(200.0f, 10.0f, 0.5f) == BiomeType::SnowPeak);
	assert(PCGBiomeGenerator::ClassifyBiome(50.0f, 45.0f, 0.5f) == BiomeType::RockyMountain);
	assert(PCGBiomeGenerator::ClassifyBiome(30.0f, 10.0f, 0.1f) == BiomeType::Desert);
	assert(PCGBiomeGenerator::ClassifyBiome(20.0f, 10.0f, 0.8f) == BiomeType::Forest);
	assert(PCGBiomeGenerator::ClassifyBiome(20.0f, 10.0f, 0.35f) == BiomeType::Meadow);

	std::cout << "[PASS] TestPCGBiomeClassification\n";
}

static void TestSplineTerrainDeformation()
{
	TerrainData terrain(64, 64, 1.0f, 1.0f);
	// 制作一个高地 (高度 20m)
	for (uint32_t z = 0; z < 64; ++z)
		for (uint32_t x = 0; x < 64; ++x)
			terrain.SetHeight(x, z, 20.0f);

	Spline3D spline;
	spline.AddPoint(glm::vec3(10.0f, 10.0f, 32.0f));
	spline.AddPoint(glm::vec3(50.0f, 10.0f, 32.0f));

	// 沿样条线开槽，挖深到 10m
	PCGBiomeGenerator::DeformTerrainAlongSpline(terrain, spline, 6.0f, 0.0f, 4.0f);

	// 验证样条线中心位置的高度已被平整削切至 10m 附近
	float hOnRoad = terrain.SampleHeight(30.0f, 32.0f);
	assert(hOnRoad < 15.0f);

	std::cout << "[PASS] TestSplineTerrainDeformation\n";
}

static void TestEQSCoverSystem()
{
	// 威胁者位于原点 (0, 0, 0)
	const glm::vec3 threatPos(0.0f, 0.0f, 0.0f);
	// 询问者位于 (20, 0, 0)
	const glm::vec3 querierPos(20.0f, 0.0f, 0.0f);

	// 在两者之间放置一堵高墙障碍物 (X: 8~12, Y: 0~4, Z: -5~5)
	std::vector<CoverBox> obstacles = {
		{ { 8.0f, 0.0f, -5.0f }, { 12.0f, 4.0f, 5.0f } }
	};

	glm::vec3 bestCover;
	float bestScore = 0.0f;
	bool found = EQSCoverSystem::FindBestCover(querierPos, threatPos, 15.0f, 1.0f, obstacles, bestCover, bestScore);

	assert(found);
	assert(bestScore > 2.0f); // 视线完全遮蔽 (> 2.0)
	// 优质掩体应该位于墙壁后方 (X > 12.0)
	assert(bestCover.x >= 12.0f);

	std::cout << "[PASS] TestEQSCoverSystem (Best Cover: "
		<< bestCover.x << ", " << bestCover.y << ", " << bestCover.z << ", Score: " << bestScore << ")\n";
}

static void TestFoliageInteractionPhysics()
{
	const glm::vec3 foliagePos(10.0f, 0.0f, 10.0f);
	const glm::vec3 pusherPos(10.5f, 0.0f, 10.0f); // 距离 0.5m
	const glm::vec3 pusherVel(2.0f, 0.0f, 0.0f);

	glm::vec3 displacement;
	FoliageInteractionSystem::ComputePushDisplacement(foliagePos, pusherPos, pusherVel, 1.5f, 1.0f, displacement);

	// 应该产生远离推挤者的位移向量
	assert(glm::length(displacement) > 0.0f);

	// 模拟弹性阻尼回弹
	glm::vec3 disp = displacement;
	for (int i = 0; i < 10; ++i)
	{
		FoliageInteractionSystem::UpdateSpringRecovery(disp, 5.0f, 0.1f);
	}
	assert(glm::length(disp) < glm::length(displacement) * 0.1f); // 显著回弹复位

	std::cout << "[PASS] TestFoliageInteractionPhysics\n";
}

static void TestPCGAndLivingAIComponentLayout()
{
	BiomePCGComponent pcg;
	assert(sizeof(pcg) == 48);
	assert(pcg.MaxSlope == 35.0f);

	EQSComponent eqs;
	assert(sizeof(eqs) == 32);
	assert(eqs.QueryRadius == 20.0f);

	FoliageInteractionComponent fi;
	assert(sizeof(fi) == 32);
	assert(fi.PushRadius == 1.5f);

	std::cout << "[PASS] TestPCGAndLivingAIComponentLayout\n";
}

int main()
{
	std::cout << "Starting World.PCGAndLivingAI tests...\n";
	TestPCGBiomeClassification();
	TestSplineTerrainDeformation();
	TestEQSCoverSystem();
	TestFoliageInteractionPhysics();
	TestPCGAndLivingAIComponentLayout();
	std::cout << "All World.PCGAndLivingAI tests passed successfully!\n";
	return 0;
}
