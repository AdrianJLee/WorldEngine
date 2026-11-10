#pragma once

#include "World/Core/Export.h"
#include "World/Renderer/Renderer3D.h"
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace World
{
	class ParticlePool;
	struct ParticleSoA;

	struct ParticleLightConfig
	{
		bool Enabled = true;
		float IntensityThreshold = 1.0f;       // Minimum brightness (luminance * EmissiveBoost) to spawn light
		float RangeMultiplier = 5.0f;          // Radius = Particle.Size * RangeMultiplier
		float IntensityScale = 1.0f;
		uint32_t MaxLights = 16;
		float MinRange = 0.5f;
		float MaxRange = 50.0f;
		bool ColorAffectsLight = true;
	};

	class WLD_API ParticleLightExtractor
	{
	public:
		explicit ParticleLightExtractor(const ParticleLightConfig& config = ParticleLightConfig {});
		~ParticleLightExtractor() = default;

		void SetConfig(const ParticleLightConfig& config) { m_Config = config; }
		const ParticleLightConfig& GetConfig() const { return m_Config; }
		ParticleLightConfig& GetConfigMut() { return m_Config; }

		// Extracts dynamic point lights from active particles in ParticlePool
		size_t ExtractLights(const ParticlePool& pool, std::vector<PointLightData>& outLights) const;

		// Extracts dynamic point lights from ParticleSoA
		size_t ExtractLightsFromSoA(
			const ParticleSoA& soa,
			float emissiveBoost,
			std::vector<PointLightData>& outLights) const;

		// Injects extracted dynamic particle lights into a target light list
		void InjectIntoLightRig(
			const ParticlePool& pool,
			std::vector<PointLightData>& targetLights,
			uint32_t maxTotalLights = 32) const;

	private:
		ParticleLightConfig m_Config;
	};
}
