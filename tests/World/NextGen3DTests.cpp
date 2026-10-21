#include "World/Renderer/MeshletBuilder.h"
#include "World/Environment/WaterSimulation.h"
#include "World/Environment/WeatherSimulation.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestMeshletClustering()
{
	// 构造一个 16x16 的大网格 (289 个顶点，512 个三角形)
	std::vector<glm::vec3> verts;
	std::vector<uint32_t> indices;

	for (int z = 0; z <= 16; ++z)
		for (int x = 0; x <= 16; ++x)
			verts.push_back(glm::vec3(x * 2.0f, std::sin(x * 0.5f) * 2.0f, z * 2.0f));

	for (int z = 0; z < 16; ++z)
	{
		for (int x = 0; x < 16; ++x)
		{
			uint32_t v0 = z * 17 + x;
			uint32_t v1 = z * 17 + (x + 1);
			uint32_t v2 = (z + 1) * 17 + (x + 1);
			uint32_t v3 = (z + 1) * 17 + x;

			indices.push_back(v0); indices.push_back(v1); indices.push_back(v2);
			indices.push_back(v0); indices.push_back(v2); indices.push_back(v3);
		}
	}

	std::vector<Meshlet> meshlets;
	std::vector<uint32_t> meshletVerts;
	std::vector<uint8_t> meshletTris;

	MeshletBuilder::BuildMeshlets(verts, indices, meshlets, meshletVerts, meshletTris, 64, 126);

	assert(!meshlets.empty());
	for (const auto& m : meshlets)
	{
		// 必须严格满足 GPU 硬件上限
		assert(m.VertexCount <= 64);
		assert(m.TriangleCount <= 126);
		assert(m.BoundingRadius > 0.0f);
		assert(m.ConeCutoff >= -1.0f && m.ConeCutoff <= 1.0f);
	}

	std::cout << "[PASS] TestMeshletClustering (Meshlets: " << meshlets.size() << ")\n";
}

static void TestWaterSimulationAndBuoyancy()
{
	WaterBodyComponent water;
	water.WaveLength = 20.0f;
	water.WaveSpeed = 3.0f;
	water.WaveSteepness = 0.5f;

	// 1. 波高采样
	float h0 = WaterSimulation::SampleWaveHeight(0.0f, 0.0f, 0.0f, water);
	float h1 = WaterSimulation::SampleWaveHeight(10.0f, 5.0f, 1.0f, water);
	assert(std::isfinite(h0));
	assert(std::isfinite(h1));

	// 2. 法线采样
	glm::vec3 n = WaterSimulation::SampleWaveNormal(0.0f, 0.0f, 0.0f, water);
	assert(std::abs(glm::length(n) - 1.0f) < 1e-3f);
	assert(n.y > 0.5f); // 总体朝上

	// 3. 浮力测试
	glm::vec3 force, torque;
	// 刚体完全沉在水下 (-2m)
	WaterSimulation::ComputeBuoyancy(glm::vec3(0.0f, -2.0f, 0.0f), glm::vec3(1.0f), 50.0f, 0.0f, water, force, torque);
	assert(force.y > 0.0f); // 产生向上浮力

	// 刚体在水面高空 (50m)，无浮力
	WaterSimulation::ComputeBuoyancy(glm::vec3(0.0f, 50.0f, 0.0f), glm::vec3(1.0f), 50.0f, 0.0f, water, force, torque);
	assert(force.y == 0.0f);

	std::cout << "[PASS] TestWaterSimulationAndBuoyancy\n";
}

static void TestWeatherSimulation()
{
	WeatherSystemComponent weather;
	weather.CurrentWeather = WeatherSystemComponent::WeatherType::Storm;

	// 1. 暴风雨天气：降水与积水累加
	for (int i = 0; i < 10; ++i)
		WeatherSimulation::Update(weather, 0.5f);

	assert(weather.PrecipitationIntensity > 0.5f);
	assert(weather.Wetness > 0.0f);

	// 2. 晴天：降水停止，积水蒸发自然风干
	weather.CurrentWeather = WeatherSystemComponent::WeatherType::Clear;
	weather.WindSpeed = 10.0f;
	for (int i = 0; i < 50; ++i)
		WeatherSimulation::Update(weather, 0.5f);

	assert(weather.PrecipitationIntensity < 0.1f);
	assert(weather.Wetness < 0.2f); // 显著蒸发减少

	std::cout << "[PASS] TestWeatherSimulation\n";
}

static void TestNextGenComponentLayout()
{
	WaterBodyComponent water;
	assert(sizeof(water) == 48);
	assert(water.WaveSteepness == 0.5f);
	assert(water.WaveLength == 20.0f);

	WeatherSystemComponent weather;
	assert(sizeof(weather) == 48);
	assert(weather.CurrentWeather == WeatherSystemComponent::WeatherType::Clear);
	assert(weather.WindSpeed == 5.0f);

	std::cout << "[PASS] TestNextGenComponentLayout\n";
}

int main()
{
	std::cout << "Starting World.NextGen3D tests...\n";
	TestMeshletClustering();
	TestWaterSimulationAndBuoyancy();
	TestWeatherSimulation();
	TestNextGenComponentLayout();
	std::cout << "All World.NextGen3D tests passed successfully!\n";
	return 0;
}
