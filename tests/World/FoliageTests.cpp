#include "World/Foliage/Foliage.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

using namespace World;

static void TestFoliageInitialization()
{
	FoliageData foliage;
	assert(foliage.GetTypeCount() >= 1);
	assert(foliage.GetTotalInstanceCount() == 0);
	assert(foliage.GetChunks().empty());

	FoliageType bush;
	bush.Name = "Test Bush";
	bush.Density = 4.0f;
	size_t idx = foliage.AddType(bush);
	assert(idx == 1);
	assert(foliage.GetTypeCount() == 2);
	assert(foliage.GetType(1).Name == "Test Bush");

	std::cout << "[PASS] TestFoliageInitialization\n";
}

static void TestFoliagePaintingWithoutTerrain()
{
	FoliageData foliage;
	const glm::vec3 center(100.0f, 0.0f, 100.0f);
	const float radius = 20.0f;
	const float density = 3.0f;

	uint32_t count = foliage.PaintFoliage(center, radius, density, 0, nullptr);
	assert(count > 0);
	assert(foliage.GetTotalInstanceCount() == count);
	assert(!foliage.GetChunks().empty());

	// 验证所有生成的实例位置落在半径范围内
	for (const auto& chunk : foliage.GetChunks())
	{
		for (const auto& inst : chunk.Instances)
		{
			glm::vec3 pos(inst.Transform[3]);
			float dist = glm::distance(glm::vec2(pos.x, pos.z), glm::vec2(center.x, center.z));
			assert(dist <= radius + 0.01f);
			assert(pos.y == center.y);
		}
	}

	std::cout << "[PASS] TestFoliagePaintingWithoutTerrain\n";
}

static void TestFoliagePaintingWithTerrainAndFilters()
{
	TerrainData terrain(64, 64, 2.0f, 1.0f);
	// 制造一个斜坡：Z 从 0 到 63，高度从 0 到 100
	for (uint32_t z = 0; z < 64; ++z)
	{
		for (uint32_t x = 0; x < 64; ++x)
		{
			terrain.SetHeight(x, z, static_cast<float>(z) * 1.5f);
		}
	}
	// 在 (32, 32) 挖一个地洞
	terrain.PaintHole(32.0f * 2.0f, 32.0f * 2.0f, 5.0f, true);

	FoliageData foliage;
	FoliageType grass = foliage.GetType(0);
	grass.SlopeLimit = glm::vec2(0.0f, 60.0f); // 允许斜坡
	grass.AlignToNormal = true;
	foliage.GetType(0) = grass;

	const glm::vec3 center(64.0f, 0.0f, 64.0f);
	uint32_t spawned = foliage.PaintFoliage(center, 25.0f, 2.0f, 0, &terrain);
	assert(spawned > 0);

	for (const auto& chunk : foliage.GetChunks())
	{
		for (const auto& inst : chunk.Instances)
		{
			glm::vec3 pos(inst.Transform[3]);
			// 必须贴合地形高度
			float expectedHeight = terrain.SampleHeight(pos.x, pos.z);
			assert(std::abs(pos.y - expectedHeight) < 0.1f);

			// 不能生成在地洞中
			uint32_t gx = static_cast<uint32_t>(pos.x / terrain.GetCellSize());
			uint32_t gz = static_cast<uint32_t>(pos.z / terrain.GetCellSize());
			assert(!terrain.IsHole(gx, gz));
		}
	}

	std::cout << "[PASS] TestFoliagePaintingWithTerrainAndFilters\n";
}

static void TestFoliageErasing()
{
	FoliageData foliage;
	const glm::vec3 center(50.0f, 0.0f, 50.0f);
	uint32_t spawned = foliage.PaintFoliage(center, 15.0f, 5.0f, 0, nullptr);
	assert(spawned > 0);

	// 在圆心擦除 10m
	uint32_t erased = foliage.EraseFoliage(center, 10.0f, 0);
	assert(erased > 0);
	assert(foliage.GetTotalInstanceCount() == spawned - erased);

	// 确认圆心 10m 内再无实例
	for (const auto& chunk : foliage.GetChunks())
	{
		for (const auto& inst : chunk.Instances)
		{
			glm::vec3 pos(inst.Transform[3]);
			float dist = glm::distance(glm::vec2(pos.x, pos.z), glm::vec2(center.x, center.z));
			assert(dist > 9.9f);
		}
	}

	std::cout << "[PASS] TestFoliageErasing\n";
}

static void TestFoliageCullingAndCollection()
{
	FoliageData foliage;
	// 在不同位置散布
	foliage.PaintFoliage(glm::vec3(0.0f, 0.0f, 0.0f), 10.0f, 2.0f, 0, nullptr);   // 近处
	foliage.PaintFoliage(glm::vec3(0.0f, 0.0f, 300.0f), 10.0f, 2.0f, 0, nullptr); // 超远 (300m)

	std::vector<FoliageInstanceData> visible;
	glm::vec3 cameraPos(0.0f, 0.0f, 0.0f);
	glm::mat4 vp(1.0f);

	foliage.CollectVisibleInstances(cameraPos, vp, 150.0f, 20.0f, visible);
	assert(!visible.empty());
	assert(visible.size() < foliage.GetTotalInstanceCount());

	for (const auto& inst : visible)
	{
		glm::vec3 pos(inst.Transform[3]);
		assert(glm::distance(pos, cameraPos) <= 150.0f);
	}

	std::cout << "[PASS] TestFoliageCullingAndCollection\n";
}

static void TestFoliageSerializationRoundtrip()
{
	const std::string tmpPath = "tests_foliage_roundtrip.wfoliage";
	FoliageData orig;
	orig.PaintFoliage(glm::vec3(25.0f, 5.0f, 25.0f), 15.0f, 3.0f, 0, nullptr);
	size_t origCount = orig.GetTotalInstanceCount();
	assert(origCount > 0);

	bool saved = orig.SaveToFile(tmpPath);
	assert(saved);

	FoliageData loaded;
	bool ok = loaded.LoadFromFile(tmpPath);
	assert(ok);
	assert(loaded.GetTypeCount() == orig.GetTypeCount());
	assert(loaded.GetTotalInstanceCount() == origCount);

	if (std::filesystem::exists(tmpPath))
	{
		std::filesystem::remove(tmpPath);
	}

	std::cout << "[PASS] TestFoliageSerializationRoundtrip\n";
}

static void GenerateSampleDefaultFoliageAsset()
{
	FoliageData foliage;
	std::filesystem::create_directories("projects/default/assets/foliage");
	foliage.PaintFoliage(glm::vec3(50.0f, 0.0f, 50.0f), 30.0f, 3.0f, 0, nullptr);
	foliage.SaveToFile("projects/default/assets/foliage/default.wfoliage");
	std::cout << "[PASS] GenerateSampleDefaultFoliageAsset\n";
}
static void TestFoliageDensitySaturation()
{
	FoliageData foliage;
	const glm::vec3 center(100.0f, 0.0f, 100.0f);
	const float radius = 10.0f;
	const float density = 2.0f;

	// Paint 100 times at the exact same location!
	for (int i = 0; i < 100; ++i)
	{
		foliage.PaintFoliage(center, radius, density, 0, nullptr);
	}

	size_t total = foliage.GetTotalInstanceCount();
	const float area = 3.1415926535f * radius * radius;
	const uint32_t maxAllowed = static_cast<uint32_t>(area * density * 0.15f);
	assert(total <= maxAllowed);
	assert(total > 0);
	std::cout << "[PASS] TestFoliageDensitySaturation (100 strokes capped at " << total << "/" << maxAllowed << ")\n";
}

int main()
{
	std::cout << "Starting World.Foliage tests...\n";
	TestFoliageInitialization();
	TestFoliagePaintingWithoutTerrain();
	TestFoliageDensitySaturation();
	TestFoliagePaintingWithTerrainAndFilters();
	TestFoliageErasing();
	TestFoliageCullingAndCollection();
	TestFoliageSerializationRoundtrip();
	GenerateSampleDefaultFoliageAsset();
	std::cout << "All World.Foliage tests passed successfully!\n";
	return 0;
}

