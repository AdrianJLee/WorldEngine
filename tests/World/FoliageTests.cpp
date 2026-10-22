#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#include "World/Foliage/Foliage.h"
#include "World/Terrain/Terrain.h"
#include "World/Scene/Components.h"
#include "World/Asset/WModelIO.h"
#include "World/Renderer/Mesh.h"

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
	assert(foliage.GetType(0).MeshAsset.Path.IsValid());
	assert(foliage.GetType(0).MaterialAsset.Path.IsValid());

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

static void GenerateFoliageModelAssets()
{
	constexpr float kPi = 3.1415926535f;

	// 1. grass_cluster.wmodel
	{
		Asset::WModelData model;
		model.Meta.Valid = true;
		model.Meta.ImporterVersion = 1;
		model.Meta.Identity = AssetId { 0x47524153530001u }; // "GRASS"
		model.Meta.HasSettings = true;
		model.Meta.Scale = 1.0f;
		model.Meta.SourcePath = "models/foliage/grass_cluster.wmodel";

		for (int q = 0; q < 3; ++q)
		{
			const float angle = static_cast<float>(q) * (kPi / 3.0f);
			const float cosA = std::cos(angle);
			const float sinA = std::sin(angle);
			const glm::vec3 dir(cosA * 0.5f, 0.0f, sinA * 0.5f);
			const glm::vec3 normal = glm::normalize(glm::vec3(-sinA * 0.4f, 0.85f, cosA * 0.4f));

			const uint32_t baseV = static_cast<uint32_t>(model.Vertices.size());
			model.Vertices.push_back({ -dir, normal, { 0.0f, 1.0f } });
			model.Vertices.push_back({  dir, normal, { 1.0f, 1.0f } });
			model.Vertices.push_back({  dir + glm::vec3(0.0f, 1.4f, 0.0f), normal, { 1.0f, 0.0f } });
			model.Vertices.push_back({ -dir + glm::vec3(0.0f, 1.4f, 0.0f), normal, { 0.0f, 0.0f } });

			model.Indices.push_back(baseV + 0);
			model.Indices.push_back(baseV + 1);
			model.Indices.push_back(baseV + 2);
			model.Indices.push_back(baseV + 0);
			model.Indices.push_back(baseV + 2);
			model.Indices.push_back(baseV + 3);
		}

		model.Bounds.Min = glm::vec3(-0.5f, 0.0f, -0.5f);
		model.Bounds.Max = glm::vec3(0.5f, 1.4f, 0.5f);
		model.Submeshes.push_back({ 0, static_cast<uint32_t>(model.Indices.size()), 0 });
		model.Meshes.push_back({ 0, 1 });
		Asset::WModelNode rootNode;
		rootNode.Name = "Root";
		rootNode.Parent = -1;
		rootNode.MeshIndex = 0;
		rootNode.Scale = glm::vec3(1.0f);
		model.Nodes.push_back(rootNode);
		model.MaterialSlots.push_back("textures/foliage/grass_tuft.png");

		const std::string relPath = "models/foliage/grass_cluster.wmodel";
		for (const auto& root : { std::string("Engine/content") })
		{
			std::filesystem::path outPath = std::filesystem::path(root) / relPath;
			std::filesystem::create_directories(outPath.parent_path());
			std::string err;
			Asset::WModelIO::WriteFile(outPath.string(), model, &err);
		}
	}

	// 2. bush_cluster.wmodel
	{
		Asset::WModelData model;
		model.Meta.Valid = true;
		model.Meta.ImporterVersion = 1;
		model.Meta.Identity = AssetId { 0x425553480001u }; // "BUSH"
		model.Meta.HasSettings = true;
		model.Meta.Scale = 1.0f;
		model.Meta.SourcePath = "models/foliage/bush_cluster.wmodel";

		auto addQuad = [&](const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3,
			const glm::vec2& uv0, const glm::vec2& uv1, const glm::vec2& uv2, const glm::vec2& uv3) {
			const uint32_t bv = static_cast<uint32_t>(model.Vertices.size());
			model.Vertices.push_back({ p0, glm::vec3(0.0f, 1.0f, 0.0f), uv0 });
			model.Vertices.push_back({ p1, glm::vec3(0.0f, 1.0f, 0.0f), uv1 });
			model.Vertices.push_back({ p2, glm::vec3(0.0f, 1.0f, 0.0f), uv2 });
			model.Vertices.push_back({ p3, glm::vec3(0.0f, 1.0f, 0.0f), uv3 });

			model.Indices.push_back(bv + 0);
			model.Indices.push_back(bv + 1);
			model.Indices.push_back(bv + 2);
			model.Indices.push_back(bv + 0);
			model.Indices.push_back(bv + 2);
			model.Indices.push_back(bv + 3);
		};

		// 8 radial outer canopy quads
		for (int q = 0; q < 8; ++q)
		{
			const float a = static_cast<float>(q) * (kPi * 0.25f) + 0.15f;
			const float cosA = std::cos(a);
			const float sinA = std::sin(a);
			const glm::vec3 centerBottom(cosA * 0.28f, 0.05f, sinA * 0.28f);
			const glm::vec3 tangent(-sinA * 0.42f, 0.0f, cosA * 0.42f);
			const glm::vec3 upOut(cosA * 0.35f, 0.88f, sinA * 0.35f);

			addQuad(centerBottom - tangent, centerBottom + tangent,
				centerBottom + tangent + upOut, centerBottom - tangent + upOut,
				{ 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f });
		}
		// 4 core cross quads
		for (int q = 0; q < 4; ++q)
		{
			const float a = static_cast<float>(q) * (kPi * 0.25f);
			const float cosA = std::cos(a);
			const float sinA = std::sin(a);
			const glm::vec3 dirVec(cosA * 0.55f, 0.0f, sinA * 0.55f);
			const glm::vec3 upVec(0.0f, 1.05f, 0.0f);

			addQuad(-dirVec + glm::vec3(0.0f, 0.05f, 0.0f), dirVec + glm::vec3(0.0f, 0.05f, 0.0f),
				dirVec + glm::vec3(0.0f, 0.05f, 0.0f) + upVec, -dirVec + glm::vec3(0.0f, 0.05f, 0.0f) + upVec,
				{ 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f });
		}
		// 2 crown dome cap quads
		for (int q = 0; q < 2; ++q)
		{
			const float a = static_cast<float>(q) * (kPi * 0.5f) + (kPi * 0.25f);
			const float cosA = std::cos(a);
			const float sinA = std::sin(a);
			const glm::vec3 rightVec(-sinA * 0.45f, 0.0f, cosA * 0.45f);
			const glm::vec3 fwdVec(cosA * 0.45f, 0.0f, sinA * 0.45f);
			const float capY = 0.98f + static_cast<float>(q) * 0.04f;

			addQuad(glm::vec3(0.0f, capY, 0.0f) - rightVec - fwdVec,
				glm::vec3(0.0f, capY, 0.0f) + rightVec - fwdVec,
				glm::vec3(0.0f, capY, 0.0f) + rightVec + fwdVec,
				glm::vec3(0.0f, capY, 0.0f) - rightVec + fwdVec,
				{ 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f });
		}

		// Spherical transferred normals
		const glm::vec3 sphereCenter(0.0f, 0.22f, 0.0f);
		for (auto& v : model.Vertices)
		{
			glm::vec3 n = v.Position - sphereCenter;
			const float len = glm::length(n);
			n = (len > 1e-5f) ? (n / len) : glm::vec3(0.0f, 1.0f, 0.0f);
			n.y = std::max(n.y, 0.20f);
			v.Normal = glm::normalize(n);
		}

		model.Bounds.Min = glm::vec3(-0.65f, 0.0f, -0.65f);
		model.Bounds.Max = glm::vec3(0.65f, 1.15f, 0.65f);
		model.Submeshes.push_back({ 0, static_cast<uint32_t>(model.Indices.size()), 0 });
		model.Meshes.push_back({ 0, 1 });
		Asset::WModelNode rootNode;
		rootNode.Name = "Root";
		rootNode.Parent = -1;
		rootNode.MeshIndex = 0;
		rootNode.Scale = glm::vec3(1.0f);
		model.Nodes.push_back(rootNode);
		model.MaterialSlots.push_back("textures/foliage/bush_leaves.png");

		const std::string relPath = "models/foliage/bush_cluster.wmodel";
		for (const auto& root : { std::string("Engine/content") })
		{
			std::filesystem::path outPath = std::filesystem::path(root) / relPath;
			std::filesystem::create_directories(outPath.parent_path());
			std::string err;
			Asset::WModelIO::WriteFile(outPath.string(), model, &err);
		}
	}

	// 3. scatter_pebble.wmodel
	{
		Asset::WModelData model;
		model.Meta.Valid = true;
		model.Meta.ImporterVersion = 1;
		model.Meta.Identity = AssetId { 0x524F434B0001u }; // "ROCK"
		model.Meta.HasSettings = true;
		model.Meta.Scale = 1.0f;
		model.Meta.SourcePath = "models/foliage/scatter_pebble.wmodel";

		struct BoulderConfig {
			float cx, cz;
			float sx, sz;
			float h;
			int sides;
			float rot;
			float seed;
		};

		const BoulderConfig configs[3] = {
			{  0.06f, -0.04f, 0.48f, 0.42f, 0.55f, 6, 0.20f, 1.0f },
			{ -0.44f,  0.24f, 0.28f, 0.26f, 0.34f, 5, 1.40f, 2.3f },
			{  0.38f,  0.28f, 0.18f, 0.16f, 0.18f, 5, 2.60f, 3.7f },
		};

		for (const auto& cfg : configs)
		{
			std::vector<glm::vec3> base(cfg.sides);
			std::vector<glm::vec3> waist(cfg.sides);
			std::vector<glm::vec3> shoulder(cfg.sides);

			for (int i = 0; i < cfg.sides; ++i)
			{
				const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(cfg.sides) + cfg.rot;
				const float rBase = 0.85f + 0.22f * std::sin(static_cast<float>(i) * 1.9f + cfg.seed);
				base[i] = glm::vec3(cfg.cx + cfg.sx * rBase * std::cos(a), 0.0f, cfg.cz + cfg.sz * rBase * std::sin(a));

				const float aW = a + 0.16f;
				const float rW = 0.95f + 0.18f * std::cos(static_cast<float>(i) * 2.1f + cfg.seed);
				waist[i] = glm::vec3(cfg.cx + cfg.sx * rW * std::cos(aW), cfg.h * 0.42f, cfg.cz + cfg.sz * rW * std::sin(aW));

				const float aS = a + 0.32f;
				const float rS = 0.52f + 0.15f * std::sin(static_cast<float>(i) * 2.6f + cfg.seed);
				shoulder[i] = glm::vec3(cfg.cx + cfg.sx * rS * std::cos(aS), cfg.h * 0.78f, cfg.cz + cfg.sz * rS * std::sin(aS));
			}

			const glm::vec3 top(cfg.cx + 0.04f * cfg.sx, cfg.h, cfg.cz - 0.04f * cfg.sz);

			auto addFacet = [&](glm::vec3 p0, glm::vec3 p1, glm::vec3 p2) {
				const uint32_t vStart = static_cast<uint32_t>(model.Vertices.size());
				glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
				const float len = glm::length(n);
				n = (len > 1e-6f) ? (n / len) : glm::vec3(0.0f, 1.0f, 0.0f);

				const glm::vec3 triCenter = (p0 + p1 + p2) * (1.0f / 3.0f);
				const glm::vec3 fromCenter = triCenter - glm::vec3(cfg.cx, 0.0f, cfg.cz);
				if (glm::dot(n, fromCenter) < 0.0f)
				{
					std::swap(p1, p2);
					n = -n;
				}

				auto makeUv = [](const glm::vec3& p) {
					return glm::vec2(p.x * 1.5f + 0.5f, p.z * 1.5f + 0.5f);
				};

				model.Vertices.push_back({ p0, n, makeUv(p0) });
				model.Vertices.push_back({ p1, n, makeUv(p1) });
				model.Vertices.push_back({ p2, n, makeUv(p2) });

				model.Indices.push_back(vStart + 0);
				model.Indices.push_back(vStart + 1);
				model.Indices.push_back(vStart + 2);
			};

			for (int i = 0; i < cfg.sides; ++i)
			{
				const int next = (i + 1) % cfg.sides;
				addFacet(base[i], waist[i], waist[next]);
				addFacet(base[i], waist[next], base[next]);
				addFacet(waist[i], shoulder[i], shoulder[next]);
				addFacet(waist[i], shoulder[next], waist[next]);
				addFacet(shoulder[i], top, shoulder[next]);
			}
		}

		model.Bounds.Min = glm::vec3(-0.85f, 0.0f, -0.65f);
		model.Bounds.Max = glm::vec3(0.75f, 0.65f, 0.65f);
		model.Submeshes.push_back({ 0, static_cast<uint32_t>(model.Indices.size()), 0 });
		model.Meshes.push_back({ 0, 1 });
		Asset::WModelNode rootNode;
		rootNode.Name = "Root";
		rootNode.Parent = -1;
		rootNode.MeshIndex = 0;
		rootNode.Scale = glm::vec3(1.0f);
		model.Nodes.push_back(rootNode);
		model.MaterialSlots.push_back("textures/terrain/rock_albedo.png");

		const std::string relPath = "models/foliage/scatter_pebble.wmodel";
		for (const auto& root : { std::string("Engine/content") })
		{
			std::filesystem::path outPath = std::filesystem::path(root) / relPath;
			std::filesystem::create_directories(outPath.parent_path());
			std::string err;
			Asset::WModelIO::WriteFile(outPath.string(), model, &err);
		}
	}
	std::cout << "[PASS] GenerateFoliageModelAssets (grass_cluster, bush_cluster, scatter_pebble written)\n";
}

static void GenerateSampleDefaultFoliageAsset()
{
	FoliageData foliage;
	foliage.Clear();
	while (foliage.GetTypeCount() > 0)
		foliage.RemoveType(0);

	FoliageType grass;
	grass.Name = "0: 翠绿草丛 (Wild Grass)";
	grass.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/grass_cluster.wmodel");
	grass.MaterialAsset.Path = StringPool::Get().InternPath("textures/foliage/grass_tuft.png");
	grass.Density = 3.0f;
	grass.ScaleMinMax = glm::vec2(0.8f, 1.3f);
	grass.RandomYaw = 360.0f;
	grass.AlignToNormal = true;
	grass.PaintingActive = true;
	grass.LayerMask = 0x01;
	foliage.AddType(grass);

	FoliageType flower;
	flower.Name = "1: 雏菊野花 (Wild Flowers)";
	flower.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/grass_cluster.wmodel");
	flower.MaterialAsset.Path = StringPool::Get().InternPath("textures/foliage/wild_flower.png");
	flower.Density = 1.0f;
	flower.ScaleMinMax = glm::vec2(0.6f, 1.0f);
	flower.RandomYaw = 360.0f;
	flower.AlignToNormal = true;
	flower.PaintingActive = true;
	flower.LayerMask = 0x01;
	foliage.AddType(flower);

	FoliageType shrub;
	shrub.Name = "2: 矮生灌木 (Forest Shrub)";
	shrub.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/bush_cluster.wmodel");
	shrub.MaterialAsset.Path = StringPool::Get().InternPath("textures/foliage/bush_leaves.png");
	shrub.Density = 0.6f;
	shrub.ScaleMinMax = glm::vec2(0.8f, 1.8f);
	shrub.RandomYaw = 360.0f;
	shrub.AlignToNormal = true;
	shrub.PaintingActive = true;
	shrub.LayerMask = 0x03;
	foliage.AddType(shrub);

	FoliageType rock;
	rock.Name = "3: 地表碎石 (Scatter Rocks)";
	rock.MeshAsset.Path = StringPool::Get().InternPath("models/foliage/scatter_pebble.wmodel");
	rock.MaterialAsset.Path = StringPool::Get().InternPath("textures/terrain/rock_albedo.png");
	rock.Density = 0.4f;
	rock.ScaleMinMax = glm::vec2(0.5f, 2.0f);
	rock.RandomYaw = 360.0f;
	rock.AlignToNormal = true;
	rock.PaintingActive = true;
	rock.LayerMask = 0x0E;
	foliage.AddType(rock);

	foliage.PaintFoliageMulti(glm::vec3(50.0f, 0.0f, 50.0f), 30.0f, 3.0f, { 0, 1, 2, 3 }, nullptr);

	std::filesystem::create_directories("projects/default/assets/foliage");
	foliage.SaveToFile("projects/default/assets/foliage/default.wfoliage");
	std::filesystem::create_directories("templates/project-example/assets/foliage");
	foliage.SaveToFile("templates/project-example/assets/foliage/default.wfoliage");
	std::cout << "[PASS] GenerateSampleDefaultFoliageAsset (4 species populated)\n";
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
#if defined(_WIN32) && defined(_DEBUG)
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
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
	GenerateFoliageModelAssets();
	GenerateSampleDefaultFoliageAsset();
	std::cout << "All World.Foliage tests passed successfully!\n";
	return 0;
}

