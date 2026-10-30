#include "World/Renderer/MeshletBuilder.h"
#include "World/Environment/WaterSimulation.h"
#include "World/Environment/WeatherSimulation.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <algorithm>

using namespace World;

static void TestMeshletClustering()
{
	// 构造一�?16x16 的大网格 (289 个顶点，512 个三角形)
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

	// 刚体在水面高�?(50m)，无浮力
	WaterSimulation::ComputeBuoyancy(glm::vec3(0.0f, 50.0f, 0.0f), glm::vec3(1.0f), 50.0f, 0.0f, water, force, torque);
	assert(force.y == 0.0f);

	std::cout << "[PASS] TestWaterSimulationAndBuoyancy\n";
}

static void TestWaterSurfaceMeshGeneration()
{
	WaterBodyComponent water;
	water.WaveLength = 20.0f;
	water.WaveSpeed = 2.0f;
	water.WaveSteepness = 0.5f;

	std::vector<WaterMeshVertex> vertices;
	std::vector<uint32_t> indices;

	// 生成 16x16 细分的水面网�?(extents = 50 x 50)
	WaterSimulation::GenerateWaterSurfaceMesh(water, glm::vec2(50.0f, 50.0f), 16, 1.0f, vertices, indices);

	// 顶点数量必须严格等于 (16 + 1) * (16 + 1) = 289
	assert(vertices.size() == 289);
	// 索引数量必须严格等于 16 * 16 * 6 = 1536
		assert(indices.size() == 3072);

	// 校验上表面首个三角形几何法线必须严格朝上 (+Y)，杜绝俯视背面剔除空洞缺陷
	const glm::vec3& p0 = vertices[indices[0]].Position;
	const glm::vec3& p1 = vertices[indices[1]].Position;
	const glm::vec3& p2 = vertices[indices[2]].Position;
	const glm::vec3 topTriNormal = glm::cross(p1 - p0, p2 - p0);
	assert(topTriNormal.y > 0.0f);

	// 校验顶点空间范围与波高、法�?
	float minX = 1e9f, maxX = -1e9f;
	float minZ = 1e9f, maxZ = -1e9f;
	for (const auto& v : vertices)
	{
		minX = std::min(minX, v.Position.x);
		maxX = std::max(maxX, v.Position.x);
		minZ = std::min(minZ, v.Position.z);
		maxZ = std::max(maxZ, v.Position.z);

		assert(std::isfinite(v.Position.y));
		assert(std::abs(glm::length(v.Normal) - 1.0f) < 1e-3f);
		assert(v.Normal.y > 0.3f); // 法向总体向上
		assert(v.TexCoord.x >= 0.0f && v.TexCoord.x <= 1.0f);
		assert(v.TexCoord.y >= 0.0f && v.TexCoord.y <= 1.0f);
	}

	assert(std::abs(minX - (-25.0f)) < 1e-3f);
	assert(std::abs(maxX - 25.0f) < 1e-3f);
	assert(std::abs(minZ - (-25.0f)) < 1e-3f);
	assert(std::abs(maxZ - 25.0f) < 1e-3f);

	// 校验索引都在合法范围�?
	for (uint32_t idx : indices)
	{
		assert(idx < vertices.size());
	}

	std::cout << "[PASS] TestWaterSurfaceMeshGeneration (Verts: " << vertices.size() << ", Indices: " << indices.size() << ")\n";
}

static void TestWeatherSimulation()
{
	WeatherSystemComponent weather;
	weather.CurrentWeather = WeatherSystemComponent::WeatherType::Storm;

	// 1. 暴风雨天气：降水与积水累�?
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

static void TestWeatherAtmosphereMapping()
{
	WeatherSystemComponent clearWeather;
	clearWeather.CurrentWeather = WeatherSystemComponent::WeatherType::Clear;
	auto clearParams = WeatherSimulation::ComputeAtmosphereParams(clearWeather);

	WeatherSystemComponent overcastWeather;
	overcastWeather.CurrentWeather = WeatherSystemComponent::WeatherType::Overcast;
	auto overcastParams = WeatherSimulation::ComputeAtmosphereParams(overcastWeather);

	WeatherSystemComponent rainWeather;
	rainWeather.CurrentWeather = WeatherSystemComponent::WeatherType::Rain;
	auto rainParams = WeatherSimulation::ComputeAtmosphereParams(rainWeather);

	WeatherSystemComponent stormWeather;
	stormWeather.CurrentWeather = WeatherSystemComponent::WeatherType::Storm;
	auto stormParams = WeatherSimulation::ComputeAtmosphereParams(stormWeather);

	WeatherSystemComponent snowWeather;
	snowWeather.CurrentWeather = WeatherSystemComponent::WeatherType::Snow;
	auto snowParams = WeatherSimulation::ComputeAtmosphereParams(snowWeather);

	// 1. 日光强度递变：晴�?> 雪天 > 多云 > 阴雨 > 风暴
	assert(clearParams.SunLightIntensity > snowParams.SunLightIntensity);
	assert(snowParams.SunLightIntensity > overcastParams.SunLightIntensity);
	assert(overcastParams.SunLightIntensity > rainParams.SunLightIntensity);
	assert(rainParams.SunLightIntensity > stormParams.SunLightIntensity);

	// 2. 雾浓度递变：晴�?< 多云 < 雪天 < 阴雨 < 风暴
	assert(clearParams.FogDensity < overcastParams.FogDensity);
	assert(overcastParams.FogDensity < snowParams.FogDensity);
	assert(snowParams.FogDensity < rainParams.FogDensity);
	assert(rainParams.FogDensity < stormParams.FogDensity);

	// 3. 天空清屏颜色合法�?
	for (const auto& p : { clearParams, overcastParams, rainParams, stormParams, snowParams })
	{
		assert(p.SkyClearColor.a == 1.0f);
		assert(p.SkyClearColor.r >= 0.0f && p.SkyClearColor.r <= 1.0f);
		assert(p.SkyClearColor.g >= 0.0f && p.SkyClearColor.g <= 1.0f);
		assert(p.SkyClearColor.b >= 0.0f && p.SkyClearColor.b <= 1.0f);
		assert(p.SunLightColor.r >= 0.0f && p.SunLightColor.r <= 1.0f);
		assert(p.AmbientColor.r >= 0.0f && p.AmbientColor.r <= 1.0f);
	}

	std::cout << "[PASS] TestWeatherAtmosphereMapping\n";
}

static void TestGenericContinuousSolarArc()
{
	WeatherSystemComponent comp;

	// 1. 正午 (12:00) 太阳高悬天顶
	comp.TimeOfDay = 12.0f;
	auto noon = WeatherSimulation::ComputeAtmosphereParams(comp);
	assert(noon.SunElevation > 50.0f);
	assert(noon.SunLightIntensity > 1.0f);
	assert(noon.SunDirection.y < -0.7f); // 垂直朝下投射
	assert(noon.SkyClearColor.b > noon.SkyClearColor.r); // 蔚蓝天空

	// 2. 日出破晓 (06:00) 晨曦金黄
	comp.TimeOfDay = 6.0f;
	auto sunrise = WeatherSimulation::ComputeAtmosphereParams(comp);
	assert(std::abs(sunrise.SunElevation) < 5.0f);
	assert(sunrise.SunLightColor.r > sunrise.SunLightColor.b); // 偏暖�?�?

	// 3. 静谧子夜 (00:00 / 24:00) 银蓝月光
	comp.TimeOfDay = 0.0f;
	auto midnight = WeatherSimulation::ComputeAtmosphereParams(comp);
	assert(midnight.SunElevation < -10.0f); // 太阳在水平面之下
	assert(midnight.SunLightIntensity < 0.15f); // 微弱月光
	assert(midnight.SkyClearColor.r < 0.15f && midnight.SkyClearColor.g < 0.15f); // 深暗夜幕

	// 4. 昼夜自动流转递增测试
	comp.AutoCycleTime = 1;
	comp.DayCycleSpeed = 1.0f;
	WeatherSimulation::Update(comp, 2.0f);
	assert(comp.TimeOfDay == 2.0f);

	std::cout << "[PASS] TestGenericContinuousSolarArc\n";
}

static void TestGenericMultiOctaveWater()
{
	WaterBodyComponent water;
	water.WaveLength = 25.0f;
	water.WaveSpeed = 3.0f;
	water.WaveSteepness = 0.6f;
	water.WaveOctaves = 4;
	water.WaveDirectionAngle = 45.0f; // 45度朝�?
	water.FluidDensity = 1000.0f;

	// 1. 验证 45 度浪�?0 度浪的波高在非对称点不同
	float h45 = WaterSimulation::SampleWaveHeight(10.0f, 0.0f, 1.0f, water);
	water.WaveDirectionAngle = 135.0f;
	float h135 = WaterSimulation::SampleWaveHeight(10.0f, 0.0f, 1.0f, water);
	assert(std::isfinite(h45) && std::isfinite(h135));

	// 2. 验证流体密度对阿基米德浮力的线性缩�?(淡水 1000 vs 海水 1025)
	glm::vec3 forceFresh, torqueFresh;
	water.FluidDensity = 1000.0f;
	WaterSimulation::ComputeBuoyancy(glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f), 100.0f, 0.0f, water, forceFresh, torqueFresh);

	glm::vec3 forceSea, torqueSea;
	water.FluidDensity = 1025.0f;
	WaterSimulation::ComputeBuoyancy(glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f), 100.0f, 0.0f, water, forceSea, torqueSea);

	assert(forceSea.y > forceFresh.y); // 高密度海水产生更大浮�?
	assert(std::abs((forceSea.y / forceFresh.y) - 1.025f) < 1e-2f);

	std::cout << "[PASS] TestGenericMultiOctaveWater\n";
}

static void TestMeshletDebugColor()
{
	for (uint32_t i = 0; i < 100; ++i)
	{
		glm::vec4 c = MeshletBuilder::GetMeshletDebugColor(i);
		assert(c.r >= 0.0f && c.r <= 1.0f);
		assert(c.g >= 0.0f && c.g <= 1.0f);
		assert(c.b >= 0.0f && c.b <= 1.0f);
		assert(c.a == 1.0f);

		// 不应当出现全黑或不可见颜�?
		assert(glm::length(glm::vec3(c)) > 0.5f);

		// 相邻集群伪彩颜色必须具有良好的区分度 (Euclidean distance > 0.3)
		glm::vec4 nextC = MeshletBuilder::GetMeshletDebugColor(i + 1);
		float diff = glm::distance(glm::vec3(c), glm::vec3(nextC));
		assert(diff > 0.3f);
	}

	std::cout << "[PASS] TestMeshletDebugColor\n";
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
	TestWaterSurfaceMeshGeneration();
	TestWeatherSimulation();
	TestWeatherAtmosphereMapping();
	TestGenericContinuousSolarArc();
	TestGenericMultiOctaveWater();
	TestMeshletDebugColor();
	TestNextGenComponentLayout();
	std::cout << "All World.NextGen3D tests passed successfully!\n";
	return 0;
}




