#pragma once

#include "World/Core/Export.h"
#include "World/Core/AssetRef.h"
#include "World/Terrain/Terrain.h"
#include "World/Level/Spline3D.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace World
{
	class FoliageData;

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

	struct WLD_API PCGFilterRange
	{
		float Min { 0.0f };
		float Max { 1.0f };
		float Falloff { 0.05f };
		float EvaluateWeight(float value) const;
	};

	struct WLD_API PCGSpawnEntry
	{
		std::string Name;
		AssetRef MeshAsset;
		AssetRef MaterialAsset;
		float SelectionWeight { 1.0f };
		glm::vec2 ScaleRange { 0.85f, 1.25f };
		float AlignToNormal { 0.25f };
		float FootprintRadius { 0.45f };
		float Stiffness { 0.5f };
		bool EnablePhysicalInteraction { true };
	};

	struct WLD_API PCGBiomeLayerDesc
	{
		std::string LayerName;
		BiomeType Category { BiomeType::Meadow };
		PCGFilterRange ElevationBand { 0.0f, 1.0f, 0.08f };
		PCGFilterRange SlopeDegrees { 0.0f, 45.0f, 5.0f };
		PCGFilterRange MoistureBand { 0.0f, 1.0f, 0.08f };
		float Priority { 1.0f };
		std::vector<PCGSpawnEntry> Entries;
	};

	struct WLD_API PCGRuleGraphDesc
	{
		float SampleSpacing { 2.5f };
		float JitterFactor { 0.85f };
		bool UseNormalizedElevation { true };
		float SnowLineElevation { 150.0f };
		float SnowLineNormalized { 0.72f };
		float RockSlopeThresholdDeg { 35.0f };
		float AridMoistureThreshold { 0.25f };
		bool IsCustom { false };
		std::vector<PCGBiomeLayerDesc> Layers;

		static PCGRuleGraphDesc CreateAdaptiveDefault(const BiomePCGComponent& comp, float terrainMinY = 0.0f, float terrainMaxY = 200.0f);
	};

	class WLD_API PCGBiomeGenerator
	{
	public:
		// 全局共享可编辑规则图与实时预览点云
		static PCGRuleGraphDesc& GetMutableRuleGraph();
		static const PCGRuleGraphDesc& GetActiveRuleGraph();
		static void ResetRuleGraphToDefault(const BiomePCGComponent& comp);

		static std::vector<PCGSpawnPoint>& GetSharedPreviewPoints();
		static bool IsSharedPreviewActive();
		static void SetSharedPreviewActive(bool active);
		// 多因子生态决策规则 (完全向后兼容已有签名)
		static BiomeType ClassifyBiome(float elevation, float slopeDeg, float moisture);

		// 自适应归一化多因子生态决策规则
		static BiomeType ClassifyBiomeAdaptive(float elevation, float normalizedElev, float slopeDeg, float moisture, const BiomePCGComponent& comp);

		// 基于地形高度、坡度与湿度生成植被群落散布点 (支持自然抖动与地形世界变换)
		static void GenerateBiomePoints(
			const TerrainData& terrain,
			const BiomePCGComponent& comp,
			std::vector<PCGSpawnPoint>& outPoints,
			const glm::mat4& terrainWorld = glm::mat4(1.0f),
			const std::vector<Spline3D>* roadSplines = nullptr,
			float roadClearWidth = 6.0f);

		// 沿样条线道路对地貌高度场自动切槽挖填与放坡平滑过渡 (支持样条到地形局部坐标投影)
		static void DeformTerrainAlongSpline(
			TerrainData& terrain,
			const Spline3D& spline,
			float roadWidth,
			float cutDepth,
			float slopeTransition,
			const glm::mat4& terrainWorld = glm::mat4(1.0f));

		// 将生态散布点按物种规则程序化填充到 FoliageData 实例层中
		static uint32_t PopulateFoliageFromBiomePoints(
			const std::vector<PCGSpawnPoint>& points,
			FoliageData& foliageData,
			float scale = 1.0f,
			bool preserveHandPainted = true);

		// 沿样条道路清除指定宽度内的植被实例，形成净空道路通道
		static uint32_t ClearFoliageAlongSpline(
			FoliageData& foliageData,
			const Spline3D& spline,
			float clearWidth);
	};
}
