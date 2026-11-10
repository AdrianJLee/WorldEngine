#pragma once

#include "World/Core/Export.h"
#include "World/Core/AssetRef.h"
#include "World/Terrain/Terrain.h"
#include "World/Level/Spline3D.h"
#include "World/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace World
{
	class FoliageData;

	// 兼容向后旧结构
	struct PCGSpawnPoint
	{
		glm::vec3 Position { 0.0f };
		glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
		uint32_t LayerId { 0 };
		float Weight { 1.0f };
		glm::vec4 ColorTag { 0.2f, 0.85f, 0.45f, 0.9f };
		std::string MeshPath;
		std::string MaterialPath;
		float Scale { 1.0f };
	};

	// 工业级通用空间生成点模型 (SIMD友好，携带姿态与局部包围)
	struct WLD_API PCGPoint
	{
		glm::vec3 Position { 0.0f };
		float Density { 1.0f };
		glm::quat Rotation { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale { 1.0f };
		float Steepness { 0.0f };
		glm::vec3 Normal { 0.0f, 1.0f, 0.0f };
		float Seed { 0.0f };
		glm::vec3 BoundsMin { -0.5f };
		glm::vec3 BoundsMax { 0.5f };
		uint32_t PointId { 0 };
		uint32_t LayerIndex { 0 };
		glm::vec4 ColorTag { 0.2f, 0.85f, 0.45f, 0.9f };
		std::string MeshPath;
		std::string MaterialPath;
	};

	// 通用动态属性集
	class WLD_API PCGAttributeSet
	{
	public:
		void SetFloat(const std::string& name, float value);
		float GetFloat(const std::string& name, float defaultValue = 0.0f) const;
		bool HasFloat(const std::string& name) const;

		void SetString(const std::string& name, const std::string& value);
		std::string GetString(const std::string& name, const std::string& defaultValue = "") const;
		bool HasString(const std::string& name) const;

		void Clear();
		const std::unordered_map<std::string, float>& GetAllFloats() const { return m_Floats; }
		const std::unordered_map<std::string, std::string>& GetAllStrings() const { return m_Strings; }

	private:
		std::unordered_map<std::string, float> m_Floats;
		std::unordered_map<std::string, std::string> m_Strings;
	};

	// 通用空间点云数据集
	struct WLD_API PCGPointData
	{
		std::vector<PCGPoint> Points;
		PCGAttributeSet GlobalAttributes;
		glm::mat4 WorldTransform { 1.0f };
		glm::vec3 BoundsMin { -100.0f };
		glm::vec3 BoundsMax { 100.0f };
		void Clear() { Points.clear(); GlobalAttributes.Clear(); }
	};

	struct WLD_API PCGFilterRange
	{
		float Min { 0.0f };
		float Max { 1.0f };
		float Falloff { 0.05f };
		float EvaluateWeight(float value) const;
	};

	// 用户自主添加的模型生成条目 (无任何写死默认)
	struct WLD_API PCGSpawnEntry
	{
		std::string Name { "New Entry" };
		AssetRef MeshAsset;
		AssetRef MaterialAsset;
		float SelectionWeight { 1.0f };
		glm::vec2 ScaleRange { 0.85f, 1.25f };
		float AlignToNormal { 0.25f };
		float FootprintRadius { 0.5f };
		float Stiffness { 0.5f };
		bool EnablePhysicalInteraction { false };
	};

	// 用户自定义的生成规则图层 (无任何写死分类)
	struct WLD_API PCGBiomeLayerDesc
	{
		std::string LayerName { "Custom Layer" };
		glm::vec4 ColorTag { 0.25f, 0.85f, 0.45f, 0.90f };
		bool Enabled { true };
		float Priority { 1.0f };
		PCGFilterRange ElevationBand { -5000.0f, 5000.0f, 0.0f };
		PCGFilterRange SlopeDegrees { 0.0f, 90.0f, 0.0f };
		PCGFilterRange DensityMask { 0.0f, 1.0f, 0.0f };
		std::vector<PCGSpawnEntry> Entries;
	};

	// 纯净通用规则图描述 (默认 0 个预设图层)
	struct WLD_API PCGRuleGraphDesc
	{
		float SampleSpacing { 2.5f };
		float JitterFactor { 0.85f };
		bool IsCustom { true };
		std::vector<PCGBiomeLayerDesc> Layers;

		// 构建纯净空白画布 (0 个图层，0 个预设物种)
		static PCGRuleGraphDesc CreateBlank();

		bool SaveToFile(const std::string& physicalPath, std::string* error = nullptr) const;
		bool LoadFromFile(const std::string& physicalPath, std::string* error = nullptr);
		std::string ToYamlString() const;
		bool FromYamlString(const std::string& yamlStr, std::string* error = nullptr);
	};

	// 独立 PCG Graph 资产类 (.wpcggraph)
	class WLD_API PCGGraphAsset
	{
	public:
		std::string Name { "BlankPCGGraph" };
		uint32_t Version { 2 };
		PCGRuleGraphDesc RuleGraph;

		PCGGraphAsset() = default;

		bool SaveToFile(const std::string& physicalPath, std::string* error = nullptr) const;
		bool LoadFromFile(const std::string& physicalPath, std::string* error = nullptr);

		std::string ToYamlString() const;
		bool FromYamlString(const std::string& yamlStr, std::string* error = nullptr);

		static std::shared_ptr<PCGGraphAsset> GetOrLoad(const std::string& logicalOrPhysicalPath);
		static void ClearCache();

	private:
		static std::unordered_map<std::string, std::shared_ptr<PCGGraphAsset>> s_AssetCache;
	};

	class WLD_API PCGBiomeGenerator
	{
	public:
		static PCGRuleGraphDesc& GetMutableRuleGraph();
		static const PCGRuleGraphDesc& GetActiveRuleGraph();
		static void ResetRuleGraphToDefault(const BiomePCGComponent& comp);

		static std::vector<PCGSpawnPoint>& GetSharedPreviewPoints();
		static bool IsSharedPreviewActive();
		static void SetSharedPreviewActive(bool active);

		// 基于纯净规则图与空间范围生成点云
		static void GenerateBiomePoints(
			const TerrainData& terrain,
			const BiomePCGComponent& comp,
			std::vector<PCGSpawnPoint>& outPoints,
			const glm::mat4& terrainWorld = glm::mat4(1.0f),
			const std::vector<Spline3D>* roadSplines = nullptr,
			float roadClearWidth = 6.0f,
			const PCGRuleGraphDesc* customGraph = nullptr);

		// 通用点云生成接口
		static void GeneratePointData(
			const TerrainData& terrain,
			const BiomePCGComponent& comp,
			PCGPointData& outData,
			const glm::mat4& terrainWorld = glm::mat4(1.0f),
			const std::vector<Spline3D>* roadSplines = nullptr,
			float roadClearWidth = 6.0f,
			const PCGRuleGraphDesc* customGraph = nullptr);

		// 样条道路地形自动挖填平整与放坡
		static void DeformTerrainAlongSpline(
			TerrainData& terrain,
			const Spline3D& spline,
			float roadWidth,
			float cutDepth,
			float slopeTransition,
			const glm::mat4& terrainWorld = glm::mat4(1.0f));

		// 将生成点按用户配置的条目烘焙到 FoliageData (纯数据驱动，绝无写死默认植物)
		static uint32_t PopulateFoliageFromBiomePoints(
			const std::vector<PCGSpawnPoint>& points,
			FoliageData& foliageData,
			float scale = 1.0f,
			bool preserveHandPainted = true);

		// 样条净空通道清除
		static uint32_t ClearFoliageAlongSpline(
			FoliageData& foliageData,
			const Spline3D& spline,
			float clearWidth);
	};
}
