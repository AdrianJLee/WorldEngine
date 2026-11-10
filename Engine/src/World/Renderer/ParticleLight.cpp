#include "wldpch.h"
#include "World/Renderer/ParticleLight.h"
#include "World/Renderer/ParticleSystem.h"
#include "World/Renderer/ParticleBuffer.h"

#include <algorithm>

namespace World
{
	namespace
	{
		inline float ComputeLuminance(const glm::vec4& color)
		{
			return (0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b) * color.a;
		}
	}

	ParticleLightExtractor::ParticleLightExtractor(const ParticleLightConfig& config)
		: m_Config(config)
	{
	}

	size_t ParticleLightExtractor::ExtractLights(const ParticlePool& pool, std::vector<PointLightData>& outLights) const
	{
		outLights.clear();
		if (!m_Config.Enabled || m_Config.MaxLights == 0)
			return 0;

		const auto& particles = pool.GetParticles();
		const float emissiveBoost = pool.GetMaterialSettings().EmissiveBoost;

		std::vector<PointLightData> candidates;
		candidates.reserve(m_Config.MaxLights * 2);

		for (const auto& p : particles)
		{
			if (!p.Active || p.Life <= 0.0f)
				continue;

			float lum = ComputeLuminance(p.Color);
			float effectiveIntensity = lum * emissiveBoost * m_Config.IntensityScale;

			if (effectiveIntensity >= m_Config.IntensityThreshold)
			{
				PointLightData light;
				light.Position = p.Position;
				light.Color = m_Config.ColorAffectsLight ? glm::vec3(p.Color) : glm::vec3(1.0f);
				light.Intensity = effectiveIntensity;

				float avgSize = (p.Size.x + p.Size.y) * 0.5f;
				light.Range = glm::clamp(avgSize * m_Config.RangeMultiplier, m_Config.MinRange, m_Config.MaxRange);

				candidates.push_back(light);
			}
		}

		// Sort by intensity descending so the brightest particles get lighting priority
		std::sort(candidates.begin(), candidates.end(), [](const PointLightData& a, const PointLightData& b) {
			return a.Intensity > b.Intensity;
		});

		size_t count = std::min<size_t>(candidates.size(), static_cast<size_t>(m_Config.MaxLights));
		outLights.assign(candidates.begin(), candidates.begin() + count);

		return outLights.size();
	}

	size_t ParticleLightExtractor::ExtractLightsFromSoA(
		const ParticleSoA& soa,
		float emissiveBoost,
		std::vector<PointLightData>& outLights) const
	{
		outLights.clear();
		if (!m_Config.Enabled || m_Config.MaxLights == 0)
			return 0;

		std::vector<PointLightData> candidates;
		candidates.reserve(m_Config.MaxLights * 2);

		for (size_t i = 0; i < soa.Capacity; ++i)
		{
			if (soa.Active[i] == 0 || soa.Life[i] <= 0.0f)
				continue;

			glm::vec4 color(soa.ColorR[i], soa.ColorG[i], soa.ColorB[i], soa.ColorA[i]);
			float lum = ComputeLuminance(color);
			float effectiveIntensity = lum * emissiveBoost * m_Config.IntensityScale;

			if (effectiveIntensity >= m_Config.IntensityThreshold)
			{
				PointLightData light;
				light.Position = glm::vec3(soa.PosX[i], soa.PosY[i], soa.PosZ[i]);
				light.Color = m_Config.ColorAffectsLight ? glm::vec3(color) : glm::vec3(1.0f);
				light.Intensity = effectiveIntensity;

				float avgSize = (soa.SizeX[i] + soa.SizeY[i]) * 0.5f;
				light.Range = glm::clamp(avgSize * m_Config.RangeMultiplier, m_Config.MinRange, m_Config.MaxRange);

				candidates.push_back(light);
			}
		}

		std::sort(candidates.begin(), candidates.end(), [](const PointLightData& a, const PointLightData& b) {
			return a.Intensity > b.Intensity;
		});

		size_t count = std::min<size_t>(candidates.size(), static_cast<size_t>(m_Config.MaxLights));
		outLights.assign(candidates.begin(), candidates.begin() + count);

		return outLights.size();
	}

	void ParticleLightExtractor::InjectIntoLightRig(
		const ParticlePool& pool,
		std::vector<PointLightData>& targetLights,
		uint32_t maxTotalLights) const
	{
		std::vector<PointLightData> particleLights;
		ExtractLights(pool, particleLights);

		for (const auto& pl : particleLights)
		{
			if (targetLights.size() >= maxTotalLights)
				break;
			targetLights.push_back(pl);
		}
	}
}
