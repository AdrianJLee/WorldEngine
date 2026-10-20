#include "World/Renderer/CSM.h"
#include "World/Renderer/Renderer3D.h"
#include "World/Scene/Components.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestCSMSplitsCalculation()
{
	float splits[4] = { 0.0f };
	CSM::CalculateSplits(0.1f, 200.0f, 0.85f, 4, splits);

	assert(splits[0] > 0.1f);
	assert(splits[1] > splits[0]);
	assert(splits[2] > splits[1]);
	assert(splits[3] > splits[2]);
	assert(std::abs(splits[3] - 200.0f) < 1e-3f);

	std::cout << "[PASS] TestCSMSplitsCalculation (Splits: "
		<< splits[0] << ", " << splits[1] << ", " << splits[2] << ", " << splits[3] << ")\n";
}

static void TestCSMCascadeMatrices()
{
	CSMConfig config;
	config.CascadeCount = 4;
	config.MaxShadowDistance = 200.0f;
	config.SplitLambda = 0.85f;
	config.ShadowResolution = 2048;

	glm::mat4 camTransform(1.0f);
	glm::vec3 lightDir(0.5f, -1.0f, 0.3f);

	std::array<CSMCascade, kMaxShadowCascades> cascades;
	CSM::BuildCascades(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, camTransform, lightDir, config, cascades);

	for (int i = 0; i < 4; ++i)
	{
		assert(cascades[i].SplitDistance > 0.0f);
		assert(cascades[i].FrustumSphere.w > 0.0f);
		// 矩阵对角线元素必须非零
		assert(cascades[i].ViewProjection[0][0] != 0.0f);
		assert(cascades[i].ViewProjection[1][1] != 0.0f);
		assert(cascades[i].ViewProjection[2][2] != 0.0f);
	}

	std::cout << "[PASS] TestCSMCascadeMatrices\n";
}

static void TestLightRigSpotLightPackaging()
{
	std::vector<DirectionalLightData> dirLights = {
		{ { 1.0f, 1.0f, 1.0f }, 1.0f, { 0.0f, -1.0f, 0.0f }, true }
	};
	std::vector<PointLightData> pointLights = {
		{ { 1.0f, 0.5f, 0.2f }, 2.0f, { 10.0f, 5.0f, 10.0f }, 15.0f }
	};
	std::vector<SpotLightData> spotLights = {
		{ { 0.2f, 0.8f, 1.0f }, 3.0f, { 0.0f, 10.0f, 0.0f }, 25.0f, { 0.0f, -1.0f, 0.0f }, 30.0f, 45.0f, false }
	};

	LightRig rig = Renderer3D::BuildLightRig(dirLights, pointLights, spotLights, nullptr, true);

	assert(rig.DirectionalLights == 1);
	assert(rig.PointLights == 1);
	assert(rig.SpotLights == 1);
	assert(rig.TotalLights == 3);
	assert(rig.ShadowCaster);

	// 验证 UBO 打包
	assert(rig.Uniforms.LightCounts.x == 1);
	assert(rig.Uniforms.LightCounts.y == 1);
	assert(rig.Uniforms.LightCounts.w == 1);

	// 聚光灯应该在 slot 2
	const auto& spotGpu = rig.Uniforms.Lights[2];
	assert(spotGpu.PositionType.w == 2.0f); // 2 = 聚光灯
	assert(spotGpu.DirectionRange.w == 25.0f); // Range
	assert(std::abs(spotGpu.SpotAngles.x - std::cos(glm::radians(30.0f))) < 1e-4f);
	assert(std::abs(spotGpu.SpotAngles.y - std::cos(glm::radians(45.0f))) < 1e-4f);

	std::cout << "[PASS] TestLightRigSpotLightPackaging\n";
}

static void TestSpotLightComponentLayout()
{
	SpotLight3DComponent comp;
	assert(sizeof(comp) == 32);
	assert(comp.InnerConeAngle == 30.0f);
	assert(comp.OuterConeAngle == 45.0f);
	assert(comp.Range == 20.0f);

	std::cout << "[PASS] TestSpotLightComponentLayout\n";
}

int main()
{
	std::cout << "Starting World.Lighting3D tests...\n";
	TestCSMSplitsCalculation();
	TestCSMCascadeMatrices();
	TestLightRigSpotLightPackaging();
	TestSpotLightComponentLayout();
	std::cout << "All World.Lighting3D tests passed successfully!\n";
	return 0;
}
