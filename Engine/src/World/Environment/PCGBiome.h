#pragma once

#include "World/Core/Export.h"
#include "World/Terrain/Terrain.h"
#include "World/Level/Spline3D.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace World
{
	enum class BiomeType : uint8_t
	{
		Forest = 0,
		Meadow = 1,
		RockyMountain = 2,
		Desert = 3,
		SnowPeak = 4
	};

	struct PCGSpawnPoint
	{
		glm::vec3 Position { 0.0f };
		glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
		BiomeType Biome { BiomeType::Meadow };
		float Weight { 1.0f };
	};

	class WLD_API PCGBiomeGenerator
	{
	public:
		// 多因子生态决策规则
		static BiomeType ClassifyBiome(float elevation, float slopeDeg, float moisture);

		// 基于地形高度、坡度与湿度生成植被群落散布点
		static void GenerateBiomePoints(
			const TerrainData& terrain,
			const BiomePCGComponent& comp,
			std::vector<PCGSpawnPoint>& outPoints);

		// 沿样条线道路对地貌高度场自动切槽挖填与放坡平滑过渡
		static void DeformTerrainAlongSpline(
			TerrainData& terrain,
			const Spline3D& spline,
			float roadWidth,
			float cutDepth,
			float slopeTransition);
	};
}
