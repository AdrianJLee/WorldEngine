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

static void TestFoliageMultiSpeciesPainting()
{
	FoliageData foliage;
	FoliageType flower;
	flower.Name = "Flower";
	flower.Density = 2.0f;
	flower.PaintingActive = true;
	foliage.AddType(flower);

	FoliageType bush;
	bush.Name = "Bush";
	bush.Density = 1.0f;
	bush.PaintingActive = false;
	foliage.AddType(bush);

	const glm::vec3 center(100.0f, 0.0f, 100.0f);
	const float radius = 20.0f;
	const float density = 4.0f;

	// 空物种列表返回 0
	assert(foliage.PaintFoliageMulti(center, radius, density, {}) == 0);

	// 未激活物种列表返回 0
	assert(foliage.PaintFoliageMulti(center, radius, density, { 2 }) == 0);

	// 单物种
	uint32_t spawnedSingle = foliage.PaintFoliageMulti(center, radius, density, { 0 });
	assert(spawnedSingle > 0);
	assert(foliage.GetInstanceCountByType(0) == spawnedSingle);
	assert(foliage.GetInstanceCountByType(1) == 0);

	// 多物种混刷
	foliage.Clear();
	uint32_t spawnedMulti = foliage.PaintFoliageMulti(center, radius, density, { 0, 1 });
	assert(spawnedMulti > 0);
	size_t c0 = foliage.GetInstanceCountByType(0);
	size_t c1 = foliage.GetInstanceCountByType(1);
	assert(c0 > 0);
	assert(c1 > 0);
	assert(c0 + c1 == foliage.GetTotalInstanceCount());

	// 混入非激活物种不产生其实例
	foliage.Clear();
	uint32_t spawnedWithInactive = foliage.PaintFoliageMulti(center, radius, density, { 0, 1, 2 });
	assert(spawnedWithInactive > 0);
	assert(foliage.GetInstanceCountByType(2) == 0);

	std::cout << "[PASS] TestFoliageMultiSpeciesPainting\n";
}

static void TestFoliageLayerMaskFiltering()
{
	TerrainData terrain(32, 32, 2.0f, 1.0f);
	// 左半边 Grass (Layer 0), 右半边 Rock (Layer 2)
	for (uint32_t z = 0; z < 32; ++z)
	{
		for (uint32_t x = 0; x < 32; ++x)
		{
			if (x < 16)
				terrain.SetSplatWeights(x, z, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
			else
				terrain.SetSplatWeights(x, z, glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
		}
	}

	FoliageData foliage;
	foliage.GetType(0).LayerMask = (1u << 0); // Grass only

	FoliageType rockLichen;
	rockLichen.Name = "Rock Lichen";
	rockLichen.Density = 2.0f;
	rockLichen.LayerMask = (1u << 2); // Rock only
	size_t rockIdx = foliage.AddType(rockLichen);

	FoliageType sandBush;
	sandBush.Name = "Sand Bush";
	sandBush.Density = 2.0f;
	sandBush.LayerMask = (1u << 3); // Sand only
	size_t sandIdx = foliage.AddType(sandBush);

	const glm::vec3 center(32.0f, 0.0f, 32.0f);
	const float radius = 30.0f;

	// 草地物种只应生成在 Layer 0 (Grass)
	uint32_t grassSpawned = foliage.PaintFoliage(center, radius, 2.0f, 0, &terrain);
	assert(grassSpawned > 0);
	for (const auto& chunk : foliage.GetChunks())
	{
		for (const auto& inst : chunk.Instances)
		{
			if (static_cast<size_t>(inst.Params.w + 0.5f) == 0)
			{
				glm::vec3 pos(inst.Transform[3]);
				uint32_t gx = std::clamp(static_cast<uint32_t>(pos.x / terrain.GetCellSize()), 0u, terrain.GetWidth() - 1);
				uint32_t gz = std::clamp(static_cast<uint32_t>(pos.z / terrain.GetCellSize()), 0u, terrain.GetHeight() - 1);
				glm::vec4 w = terrain.GetSplatWeights(gx, gz);
				assert(w.x >= 0.25f);
			}
		}
	}

	// 岩石地物种只应生成在 Layer 2 (Rock)
	uint32_t rockSpawned = foliage.PaintFoliage(center, radius, 2.0f, rockIdx, &terrain);
	assert(rockSpawned > 0);
	for (const auto& chunk : foliage.GetChunks())
	{
		for (const auto& inst : chunk.Instances)
		{
			if (static_cast<size_t>(inst.Params.w + 0.5f) == rockIdx)
			{
				glm::vec3 pos(inst.Transform[3]);
				uint32_t gx = std::clamp(static_cast<uint32_t>(pos.x / terrain.GetCellSize()), 0u, terrain.GetWidth() - 1);
				uint32_t gz = std::clamp(static_cast<uint32_t>(pos.z / terrain.GetCellSize()), 0u, terrain.GetHeight() - 1);
				glm::vec4 w = terrain.GetSplatWeights(gx, gz);
				assert(w.z >= 0.25f);
			}
		}
	}

	// 沙地物种因地形无沙地权重，生成实例数为 0
	uint32_t sandSpawned = foliage.PaintFoliage(center, radius, 2.0f, sandIdx, &terrain);
	assert(sandSpawned == 0);

	std::cout << "[PASS] TestFoliageLayerMaskFiltering\n";
}

static void TestFoliageRealignToTerrain()
{
	TerrainData terrain(32, 32, 2.0f, 1.0f);
	FoliageData foliage;
	foliage.PaintFoliage(glm::vec3(32.0f, 0.0f, 32.0f), 20.0f, 2.0f, 0, &terrain);
	assert(foliage.GetTotalInstanceCount() > 0);

	// 修改地貌高程
	for (uint32_t z = 0; z < 32; ++z)
	{
		for (uint32_t x = 0; x < 32; ++x)
		{
			terrain.SetHeight(x, z, 20.0f + static_cast<float>(z) * 0.5f);
		}
	}

	// 执行重新贴地
	foliage.RealignToTerrain(&terrain);

	for (const auto& chunk : foliage.GetChunks())
	{
		for (const auto& inst : chunk.Instances)
		{
			glm::vec3 pos(inst.Transform[3]);
			float expectedH = terrain.SampleHeight(pos.x, pos.z);
			assert(std::abs(pos.y - expectedH) < 0.05f);

			glm::vec3 normal = terrain.SampleNormal(pos.x, pos.z);
			glm::vec3 instUp = glm::normalize(glm::vec3(inst.Transform[1]));
			assert(glm::dot(instUp, normal) > 0.98f);
		}
	}

	std::cout << "[PASS] TestFoliageRealignToTerrain\n";
}

static void TestFoliageGetInstanceCountByType()
{
	FoliageData foliage;
	FoliageType bush;
	bush.Name = "Bush";
	foliage.AddType(bush);

	assert(foliage.GetInstanceCountByType(0) == 0);
	assert(foliage.GetInstanceCountByType(1) == 0);
	assert(foliage.GetInstanceCountByType(999) == 0);

	uint32_t c0 = foliage.PaintFoliage(glm::vec3(30, 0, 30), 10.0f, 2.0f, 0);
	assert(foliage.GetInstanceCountByType(0) == c0);
	assert(foliage.GetInstanceCountByType(1) == 0);

	uint32_t c1 = foliage.PaintFoliage(glm::vec3(60, 0, 60), 10.0f, 2.0f, 1);
	assert(foliage.GetInstanceCountByType(0) == c0);
	assert(foliage.GetInstanceCountByType(1) == c1);
	assert(foliage.GetTotalInstanceCount() == c0 + c1);

	foliage.EraseFoliage(glm::vec3(30, 0, 30), 15.0f, 0);
	assert(foliage.GetInstanceCountByType(0) == 0);
	assert(foliage.GetInstanceCountByType(1) == c1);

	std::cout << "[PASS] TestFoliageGetInstanceCountByType\n";
}

static void TestFoliageSerializationVersionCompatibility()
{
	// 验证 v1 格式向前兼容
	const std::string v1Path = "tests_foliage_v1_compat.wfoliage";
	{
		std::ofstream out(v1Path, std::ios::binary);
		uint32_t magic = 0x464F4C49;
		uint32_t version = 1;
		out.write(reinterpret_cast<const char*>(&magic), 4);
		out.write(reinterpret_cast<const char*>(&version), 4);
		uint32_t typeCount = 1;
		out.write(reinterpret_cast<const char*>(&typeCount), 4);
		std::string name = "OldGrass";
		uint32_t nameLen = static_cast<uint32_t>(name.size());
		out.write(reinterpret_cast<const char*>(&nameLen), 4);
		out.write(name.data(), nameLen);
		uint32_t zero = 0;
		uint64_t zero64 = 0;
		out.write(reinterpret_cast<const char*>(&zero), 4); // meshLen
		out.write(reinterpret_cast<const char*>(&zero64), 8); // meshId
		out.write(reinterpret_cast<const char*>(&zero), 4); // matLen
		out.write(reinterpret_cast<const char*>(&zero64), 8); // matId
		float density = 1.5f;
		out.write(reinterpret_cast<const char*>(&density), 4);
		glm::vec2 scale(0.8f, 1.2f);
		out.write(reinterpret_cast<const char*>(&scale), 8);
		float yaw = 360.0f, pitch = 5.0f;
		out.write(reinterpret_cast<const char*>(&yaw), 4);
		out.write(reinterpret_cast<const char*>(&pitch), 4);
		glm::vec2 slope(0.0f, 45.0f), height(-1000.0f, 1000.0f);
		out.write(reinterpret_cast<const char*>(&slope), 8);
		out.write(reinterpret_cast<const char*>(&height), 8);
		float cullDist = 150.0f, wind = 1.0f;
		out.write(reinterpret_cast<const char*>(&cullDist), 4);
		out.write(reinterpret_cast<const char*>(&wind), 4);
		uint8_t alignNorm = 1;
		out.write(reinterpret_cast<const char*>(&alignNorm), 1);
		uint32_t chunkCount = 0;
		out.write(reinterpret_cast<const char*>(&chunkCount), 4);
	}

	FoliageData v1Loaded;
	bool ok1 = v1Loaded.LoadFromFile(v1Path);
	assert(ok1);
	assert(v1Loaded.GetTypeCount() == 1);
	assert(v1Loaded.GetType(0).Name == "OldGrass");
	assert(v1Loaded.GetType(0).PaintingActive == true);
	assert(v1Loaded.GetType(0).LayerMask == 0xFFFFFFFF);
	std::filesystem::remove(v1Path);

	// 验证 v2 新字段往返保存加载
	const std::string v2Path = "tests_foliage_v2_roundtrip.wfoliage";
	FoliageData v2Orig;
	v2Orig.GetType(0).PaintingActive = false;
	v2Orig.GetType(0).LayerMask = 0x5;
	bool okSave = v2Orig.SaveToFile(v2Path);
	assert(okSave);

	FoliageData v2Loaded;
	bool ok2 = v2Loaded.LoadFromFile(v2Path);
	assert(ok2);
	assert(v2Loaded.GetType(0).PaintingActive == false);
	assert(v2Loaded.GetType(0).LayerMask == 0x5);
	std::filesystem::remove(v2Path);

	std::cout << "[PASS] TestFoliageSerializationVersionCompatibility\n";
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
	TestFoliageMultiSpeciesPainting();
	TestFoliageLayerMaskFiltering();
	TestFoliageRealignToTerrain();
	TestFoliageGetInstanceCountByType();
	TestFoliageSerializationVersionCompatibility();
	GenerateSampleDefaultFoliageAsset();
	std::cout << "All World.Foliage tests passed successfully!\n";
	return 0;
}

