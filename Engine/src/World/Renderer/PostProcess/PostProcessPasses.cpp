#include "wldpch.h"
#include "World/Renderer/PostProcess/PostProcessPasses.h"
#include "World/Scene/Components.h"

#include <algorithm>
#include <cmath>

namespace World
{
	void PostProcessPipeline::Update(Scene& scene, float dt)
	{
		auto& reg = scene.GetRegistry();
		for (auto entity : reg.view<PostProcessComponent>())
		{
			auto& pp = reg.get<PostProcessComponent>(entity);
			if (pp.FlashWhiteIntensity > 0.0f)
			{
				pp.FlashWhiteIntensity = std::max(0.0f, pp.FlashWhiteIntensity - pp.FlashWhiteDecay * dt);
			}
		}
	}

	void PostProcessPipeline::FlashWhite(Scene& scene, float intensity, float decay)
	{
		auto& reg = scene.GetRegistry();
		bool found = false;
		for (auto entity : reg.view<PostProcessComponent>())
		{
			auto& pp = reg.get<PostProcessComponent>(entity);
			pp.FlashWhiteIntensity = intensity;
			pp.FlashWhiteDecay = decay;
			found = true;
			break;
		}

		if (!found)
		{
			entt::entity e = reg.create();
			auto& pp = reg.emplace<PostProcessComponent>(e);
			pp.FlashWhiteIntensity = intensity;
			pp.FlashWhiteDecay = decay;
		}
	}

	PostProcessUniforms PostProcessPipeline::GetActiveUniforms(const Scene& scene)
	{
		PostProcessUniforms uniforms;
		const auto& reg = scene.GetRegistry();
		for (auto entity : reg.view<PostProcessComponent>())
		{
			const auto& pp = reg.get<PostProcessComponent>(entity);
			if (pp.Enabled != 0)
			{
				uniforms.FlashWhiteIntensity = pp.FlashWhiteIntensity;
				uniforms.OutlineWidth = pp.OutlineWidth;
				uniforms.OutlineColor = pp.OutlineColor;
				uniforms.DissolveProgress = pp.DissolveProgress;
				uniforms.BloomIntensity = pp.BloomIntensity;
				uniforms.BloomThreshold = pp.BloomThreshold;
				uniforms.VignetteIntensity = pp.VignetteIntensity;
				uniforms.ChromaticAberration = pp.ChromaticAberration;
				uniforms.Enabled = 1;
				return uniforms;
			}
		}
		uniforms.Enabled = 0;
		return uniforms;
	}

	glm::vec3 PostProcessPipeline::ApplyFlashWhite(const glm::vec3& baseColor, float intensity)
	{
		return glm::mix(baseColor, glm::vec3(1.0f), glm::clamp(intensity, 0.0f, 1.0f));
	}

	float PostProcessPipeline::CalculateVignette(const glm::vec2& uv, float intensity)
	{
		float len = glm::length(uv - glm::vec2(0.5f));
		return glm::smoothstep(0.7f, 0.7f - intensity * 0.45f, len);
	}

	void PostProcessPipeline::CalculateChromaticUVs(const glm::vec2& uv, float amount,
	                                               glm::vec2& outUvR, glm::vec2& outUvG, glm::vec2& outUvB)
	{
		glm::vec2 dir = uv - glm::vec2(0.5f);
		float dist = glm::length(dir);
		glm::vec2 offset = dir * (dist * amount * 0.05f);
		outUvR = uv + offset;
		outUvG = uv;
		outUvB = uv - offset;
	}
}
