#include "wldpch.h"
#include "World/Environment/PCGBiome.h"

#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		constexpr float kPi = 3.14159265358979323846f;
	}

	BiomeType PCGBiomeGenerator::ClassifyBiome(float elevation, float slopeDeg, float moisture)
	{
		if (elevation > 150.0f)
			return BiomeType::SnowPeak;

		if (slopeDeg > 40.0f)
			return BiomeType::RockyMountain;

		if (moisture < 0.25f)
			return BiomeType::Desert;

		if (moisture >= 0.5f && elevation < 100.0f)
			return BiomeType::Forest;

		return BiomeType::Meadow;
	}

	void PCGBiomeGenerator::GenerateBiomePoints(
		const TerrainData& terrain,
		const BiomePCGComponent& comp,
		std::vector<PCGSpawnPoint>& outPoints)
	{
		outPoints.clear();
		const float w = terrain.GetWorldWidth();
		const float h = terrain.GetWorldHeight();

		const float spacing = 8.0f;
		const uint32_t stepsX = static_cast<uint32_t>(w / spacing);
		const uint32_t stepsZ = static_cast<uint32_t>(h / spacing);

		for (uint32_t z = 0; z < stepsZ; ++z)
		{
			for (uint32_t x = 0; x < stepsX; ++x)
			{
				const float wx = static_cast<float>(x) * spacing;
				const float wz = static_cast<float>(z) * spacing;

				const float elev = terrain.SampleHeight(wx, wz);
				if (elev < comp.MinElevation || elev > comp.MaxElevation)
					continue;

				const glm::vec3 normal = terrain.SampleNormal(wx, wz);
				const float slopeDeg = std::acos(std::clamp(normal.y, 0.0f, 1.0f)) * (180.0f / kPi);
				if (slopeDeg > comp.MaxSlope)
					continue;

				// 过程式伪随机湿度计算
				const float moisture = 0.5f + 0.5f * std::sin(wx * 0.02f + static_cast<float>(comp.Seed)) * std::cos(wz * 0.02f);
				if (moisture < comp.MoistureThreshold)
					continue;

				PCGSpawnPoint pt;
				pt.Position = glm::vec3(wx, elev, wz);
				pt.Normal = normal;
				pt.Biome = ClassifyBiome(elev, slopeDeg, moisture);
				pt.Weight = moisture;

				outPoints.push_back(pt);
			}
		}
	}

	void PCGBiomeGenerator::DeformTerrainAlongSpline(
		TerrainData& terrain,
		const Spline3D& spline,
		float roadWidth,
		float cutDepth,
		float slopeTransition)
	{
		const float totalLen = spline.GetTotalLength();
		if (totalLen <= 1.0f || roadWidth <= 0.5f)
			return;

		const float step = roadWidth * 0.5f;
		const uint32_t numSamples = static_cast<uint32_t>(totalLen / step);

		const float totalInfluenceRadius = roadWidth * 0.5f + slopeTransition;

		for (uint32_t i = 0; i <= numSamples; ++i)
		{
			const float d = std::min(static_cast<float>(i) * step, totalLen);
			glm::vec3 splinePos, tangent;
			spline.SampleAtDistance(d, splinePos, tangent);

			const float targetY = splinePos.y - cutDepth;
			terrain.SculptFlatten(splinePos.x, splinePos.z, totalInfluenceRadius, targetY, 0.8f, TerrainBrushFalloff::Smooth);
		}
	}
}
