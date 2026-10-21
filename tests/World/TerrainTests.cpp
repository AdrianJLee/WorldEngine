#include "World/Terrain/Terrain.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

using namespace World;

static void TestTerrainInitializationAndDimensions()
{
	TerrainData terrain(128, 128, 2.0f, 1.5f);
	assert(terrain.GetWidth() == 128);
	assert(terrain.GetHeight() == 128);
	assert(terrain.GetCellSize() == 2.0f);
	assert(terrain.GetHeightScale() == 1.5f);
	assert(std::abs(terrain.GetWorldWidth() - 127.0f * 2.0f) < 1e-4f);
	assert(std::abs(terrain.GetWorldHeight() - 127.0f * 2.0f) < 1e-4f);

	assert(terrain.GetHeightBuffer().size() == 128 * 128);
	assert(terrain.GetSplatmapBuffer().size() == 128 * 128);
	assert(terrain.GetHoleBuffer().size() == 128 * 128);

	std::cout << "[PASS] TestTerrainInitializationAndDimensions\n";
}

static void TestBilinearHeightSampling()
{
	TerrainData terrain(64, 64, 1.0f, 1.0f);
	terrain.SetHeight(10, 10, 0.0f);
	terrain.SetHeight(11, 10, 10.0f);
	terrain.SetHeight(10, 11, 20.0f);
	terrain.SetHeight(11, 11, 30.0f);

	// 精确整数采样
	assert(std::abs(terrain.SampleHeight(10.0f, 10.0f) - 0.0f) < 1e-4f);
	assert(std::abs(terrain.SampleHeight(11.0f, 10.0f) - 10.0f) < 1e-4f);
	assert(std::abs(terrain.SampleHeight(10.0f, 11.0f) - 20.0f) < 1e-4f);
	assert(std::abs(terrain.SampleHeight(11.0f, 11.0f) - 30.0f) < 1e-4f);

	// 双线性中心采样 (u=0.5, v=0.5): (0 + 10 + 20 + 30) / 4 = 15.0f
	float mid = terrain.SampleHeight(10.5f, 10.5f);
	assert(std::abs(mid - 15.0f) < 1e-4f);

	// 边界外采样安全钳位
	float outLow = terrain.SampleHeight(-10.0f, -10.0f);
	assert(std::abs(outLow - terrain.GetHeight(0, 0)) < 1e-4f);
	float outHigh = terrain.SampleHeight(200.0f, 200.0f);
	assert(std::abs(outHigh - terrain.GetHeight(63, 63)) < 1e-4f);

	std::cout << "[PASS] TestBilinearHeightSampling\n";
}

static void TestCentralDifferenceNormal()
{
	TerrainData terrain(32, 32, 1.0f, 1.0f);

	// 平坦地面法线必须精确垂直向上 (0, 1, 0)
	for (uint32_t z = 0; z < 32; ++z)
	{
		for (uint32_t x = 0; x < 32; ++x)
		{
			terrain.SetHeight(x, z, 5.0f);
		}
	}
	glm::vec3 nFlat = terrain.SampleNormal(16.0f, 16.0f);
	assert(std::abs(nFlat.x) < 1e-3f);
	assert(std::abs(nFlat.y - 1.0f) < 1e-3f);
	assert(std::abs(nFlat.z) < 1e-3f);

	// 斜坡地面 (高度随 X 线性增加): 法向应偏向负 X 方向
	for (uint32_t z = 0; z < 32; ++z)
	{
		for (uint32_t x = 0; x < 32; ++x)
		{
			terrain.SetHeight(x, z, static_cast<float>(x));
		}
	}
	glm::vec3 nSlope = terrain.SampleNormal(16.0f, 16.0f);
	assert(nSlope.x < -0.5f);
	assert(nSlope.y > 0.5f);
	assert(std::abs(glm::length(nSlope) - 1.0f) < 1e-3f);

	std::cout << "[PASS] TestCentralDifferenceNormal\n";
}

static void TestTerrainRaycast()
{
	TerrainData terrain(64, 64, 1.0f, 1.0f);
	for (uint32_t z = 0; z < 64; ++z)
	{
		for (uint32_t x = 0; x < 64; ++x)
		{
			terrain.SetHeight(x, z, 10.0f);
		}
	}

	TerrainRaycastHit hit;
	// 垂直向下打在地表上
	bool ok = terrain.Raycast(glm::vec3(20.0f, 50.0f, 20.0f), glm::vec3(0.0f, -1.0f, 0.0f), 100.0f, hit);
	assert(ok);
	assert(hit.Hit);
	assert(std::abs(hit.Point.x - 20.0f) < 0.1f);
	assert(std::abs(hit.Point.y - 10.0f) < 0.1f);
	assert(std::abs(hit.Point.z - 20.0f) < 0.1f);

	// 射线朝上 (打不中地表)
	TerrainRaycastHit hitMiss;
	bool okMiss = terrain.Raycast(glm::vec3(20.0f, 50.0f, 20.0f), glm::vec3(0.0f, 1.0f, 0.0f), 100.0f, hitMiss);
	assert(!okMiss || !hitMiss.Hit);

	// 射线超出边界 (打向完全外部)
	TerrainRaycastHit hitOut;
	bool okOut = terrain.Raycast(glm::vec3(200.0f, 50.0f, 200.0f), glm::vec3(0.0f, -1.0f, 0.0f), 100.0f, hitOut);
	assert(!okOut || !hitOut.Hit);

	std::cout << "[PASS] TestTerrainRaycast\n";
}

static void TestSculptRaiseLowerAndFalloff()
{
	TerrainData terrain(64, 64, 1.0f, 1.0f);

	// 抬升笔刷
	terrain.SculptRaiseLower(32.0f, 32.0f, 10.0f, 5.0f, TerrainBrushFalloff::Smooth);
	float hCenter = terrain.GetHeight(32, 32);
	assert(hCenter > 4.5f);

	// 衰减测试：距离圆心越远，抬升量越小
	float hMid = terrain.GetHeight(32 + 5, 32);
	assert(hMid > 0.5f && hMid < hCenter);
	float hEdge = terrain.GetHeight(32 + 15, 32);
	assert(hEdge == 0.0f);

	// 下凹笔刷
	terrain.SculptRaiseLower(32.0f, 32.0f, 10.0f, -3.0f, TerrainBrushFalloff::Smooth);
	float hCenterAfterLower = terrain.GetHeight(32, 32);
	assert(hCenterAfterLower < hCenter);

	std::cout << "[PASS] TestSculptRaiseLowerAndFalloff\n";
}

static void TestSculptSmoothAndFlatten()
{
	TerrainData terrain(64, 64, 1.0f, 1.0f);
	// 制作一个尖刺
	terrain.SetHeight(32, 32, 50.0f);
	terrain.SculptSmooth(32.0f, 32.0f, 8.0f, 1.0f, TerrainBrushFalloff::Smooth);
	float hAfterSmooth = terrain.GetHeight(32, 32);
	assert(hAfterSmooth < 50.0f);

	// 压平笔刷至目标高度 12.0f
	terrain.SculptFlatten(32.0f, 32.0f, 10.0f, 12.0f, 1.0f, TerrainBrushFalloff::Constant);
	float hFlat = terrain.GetHeight(32, 32);
	assert(std::abs(hFlat - 12.0f) < 0.5f);

	std::cout << "[PASS] TestSculptSmoothAndFlatten\n";
}

static void TestSplatmapLayerPainting()
{
	TerrainData terrain(32, 32, 1.0f, 1.0f);
	// 初始状态 Layer 0 (草地) 权重为 1.0
	glm::vec4 initialWeights = terrain.GetSplatWeights(16, 16);
	assert(initialWeights.x > 0.95f);
	assert(std::abs(initialWeights.x + initialWeights.y + initialWeights.z + initialWeights.w - 1.0f) < 1e-2f);

	// 涂抹 Layer 1 (泥土)
	terrain.PaintLayer(16.0f, 16.0f, 5.0f, 1, 0.8f, TerrainBrushFalloff::Constant);
	glm::vec4 paintedWeights = terrain.GetSplatWeights(16, 16);
	assert(paintedWeights.y > 0.5f);
	// 4 层总权重必须严格归一化为 1.0
	float sum = paintedWeights.x + paintedWeights.y + paintedWeights.z + paintedWeights.w;
	assert(std::abs(sum - 1.0f) < 1e-2f);

	std::cout << "[PASS] TestSplatmapLayerPainting\n";
}

static void TestHoleCutout()
{
	TerrainData terrain(32, 32, 1.0f, 1.0f);
	assert(!terrain.IsHole(16, 16));

	// 涂抹地洞
	terrain.PaintHole(16.0f, 16.0f, 3.0f, true);
	assert(terrain.IsHole(16, 16));
	assert(terrain.IsHole(17, 16));
	assert(!terrain.IsHole(25, 25));

	// 填补地洞
	terrain.PaintHole(16.0f, 16.0f, 3.0f, false);
	assert(!terrain.IsHole(16, 16));

	std::cout << "[PASS] TestHoleCutout\n";
}

static void TestChunkMeshGeneration()
{
	TerrainData terrain(128, 128, 1.0f, 1.0f);
	uint32_t chunkCountX = terrain.GetChunkCountX(64);
	uint32_t chunkCountZ = terrain.GetChunkCountZ(64);
	assert(chunkCountX == 2);
	assert(chunkCountZ == 2);

	TerrainChunk chunk;
	terrain.GenerateChunkMesh(0, 0, 64, 0, chunk);
	assert(!chunk.Vertices.empty());
	assert(!chunk.Indices.empty());
	assert(chunk.BoundsMax.x >= chunk.BoundsMin.x);
	assert(chunk.BoundsMax.z >= chunk.BoundsMin.z);

	std::cout << "[PASS] TestChunkMeshGeneration\n";
}

static void TestTerrainSerializationRoundtrip()
{
	const std::string tmpPath = "tests_terrain_roundtrip.wterrain";
	{
		TerrainData orig(64, 64, 2.0f, 1.5f);
		orig.SetHeight(20, 20, 42.5f);
		orig.PaintLayer(20.0f, 20.0f, 4.0f, 2, 0.7f, TerrainBrushFalloff::Smooth);
		orig.SetHole(10, 10, true);

		bool saved = orig.SaveToFile(tmpPath);
		assert(saved);
	}

	{
		TerrainData loaded;
		bool ok = loaded.LoadFromFile(tmpPath);
		assert(ok);
		assert(loaded.GetWidth() == 64);
		assert(loaded.GetHeight() == 64);
		assert(loaded.GetCellSize() == 2.0f);
		assert(loaded.GetHeightScale() == 1.5f);
		assert(std::abs(loaded.GetHeight(20, 20) - 42.5f) < 1e-3f);
		assert(loaded.IsHole(10, 10));
		assert(!loaded.IsHole(0, 0));
	}

	if (std::filesystem::exists(tmpPath))
	{
		std::filesystem::remove(tmpPath);
	}

	std::cout << "[PASS] TestTerrainSerializationRoundtrip\n";
}

static void GenerateSampleDefaultTerrainAsset()
{
	TerrainData terrain(256, 256, 2.0f, 1.0f);
	terrain.GenerateProceduralLandscape(2026);
	std::filesystem::create_directories("projects/default/assets/terrains");
	terrain.SaveToFile("projects/default/assets/terrains/default.wterrain");
	std::cout << "[PASS] GenerateSampleDefaultTerrainAsset\n";
}

int main()
{
	std::cout << "Starting World.Terrain tests...\n";
	TestTerrainInitializationAndDimensions();
	TestBilinearHeightSampling();
	TestCentralDifferenceNormal();
	TestTerrainRaycast();
	TestSculptRaiseLowerAndFalloff();
	TestSculptSmoothAndFlatten();
	TestSplatmapLayerPainting();
	TestHoleCutout();
	TestChunkMeshGeneration();
	TestTerrainSerializationRoundtrip();
	GenerateSampleDefaultTerrainAsset();
	std::cout << "All World.Terrain tests passed successfully!\n";
	return 0;
}

