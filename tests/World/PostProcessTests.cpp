#include "World/Renderer/PostProcess/PostProcessPasses.h"
#include "World/Scene/Components.h"
#include "World/Scene/Scene.h"
#include "World/Scene/Entity.h"
#include "World/Core/Log.h"
#include "World/Core/WorldContext.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace World;

static void TestPostProcessDefaultsAndUniforms()
{
	WorldContext context;
	Scene scene(context);

	Entity ppEnt = Entity::CreateEntity(&scene, "PostProcessVolume");
	auto& pp = ppEnt.AddComponent<PostProcessComponent>();
	pp.FlashWhiteIntensity = 1.0f;
	pp.FlashWhiteDecay = 4.0f;
	pp.VignetteIntensity = 0.5f;

	PostProcessUniforms uniforms = PostProcessPipeline::GetActiveUniforms(scene);
	assert(uniforms.Enabled == 1);
	assert(uniforms.FlashWhiteIntensity == 1.0f);
	assert(uniforms.VignetteIntensity == 0.5f);

	PostProcessPipeline::Update(scene, 0.1f);
	PostProcessUniforms updatedUniforms = PostProcessPipeline::GetActiveUniforms(scene);
	assert(std::abs(updatedUniforms.FlashWhiteIntensity - 0.6f) < 1e-4f);

	std::cout << "[PASS] TestPostProcessDefaultsAndUniforms\n";
}

static void TestPostProcessMath()
{
	glm::vec3 dark(0.2f, 0.2f, 0.2f);
	glm::vec3 fullFlash = PostProcessPipeline::ApplyFlashWhite(dark, 1.0f);
	assert(std::abs(fullFlash.r - 1.0f) < 1e-5f);
	assert(std::abs(fullFlash.g - 1.0f) < 1e-5f);
	assert(std::abs(fullFlash.b - 1.0f) < 1e-5f);

	glm::vec3 noFlash = PostProcessPipeline::ApplyFlashWhite(dark, 0.0f);
	assert(std::abs(noFlash.r - 0.2f) < 1e-5f);

	float vigCenter = PostProcessPipeline::CalculateVignette(glm::vec2(0.5f, 0.5f), 0.8f);
	assert(vigCenter >= 0.99f);

	glm::vec2 r, g, b;
	PostProcessPipeline::CalculateChromaticUVs(glm::vec2(0.5f, 0.5f), 1.0f, r, g, b);
	assert(std::abs(r.x - 0.5f) < 1e-5f && std::abs(b.x - 0.5f) < 1e-5f);

	PostProcessPipeline::CalculateChromaticUVs(glm::vec2(1.0f, 0.5f), 1.0f, r, g, b);
	assert(r.x > 1.0f && b.x < 1.0f);

	std::cout << "[PASS] TestPostProcessMath\n";
}

int main()
{
	Log::Init();
	std::cout << "Running WorldPostProcessTests...\n";

	TestPostProcessDefaultsAndUniforms();
	TestPostProcessMath();

	std::cout << "All WorldPostProcessTests passed successfully.\n";
	return 0;
}
