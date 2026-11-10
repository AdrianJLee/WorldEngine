#pragma once

#include "World/Core/Export.h"
#include "World/Level/AI/EQS/EQSTypes.h"
#include "World/Level/AI/EQS/EQSQuery.h"
#include "World/Level/AI/EQS/EQSGenerator.h"
#include "World/Level/AI/EQS/EQSTest.h"
#include "World/Level/AI/EQS/EQSContext.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace World::EQS
{
	struct WLD_API EQSGeneratorDesc
	{
		std::string Type{ "ProjectedGrid" }; // "ProjectedGrid", "Circle", "AroundObstacles"
		float Radius{ 20.0f };
		float Spacing{ 2.0f };
		float InnerRadius{ 5.0f };
		float OuterRadius{ 25.0f };
		float StepDistance{ 1.5f };
		float WallOffset{ 0.75f };
		float ConeAngle{ 90.0f };
		bool IncludeGrid{ true };
		bool ProjectToTerrain{ true };
	};

	struct WLD_API EQSTestDesc
	{
		std::string Name;
		std::string TestType{ "Trace" }; // Distance, Trace, Dot, WallProximity, Height, Overlap, FriendlyPenalty, CoverReservation, Scripted
		std::string ContextType{ "Threat" }; // Querier, Target/Threat, AllThreats, AllAllies, CustomLocation
		TestPurpose Purpose{ TestPurpose::FilterAndScore };
		ScoringCurve Curve{ ScoringCurve::Linear };
		float Weight{ 1.0f };
		float FilterMin{ 0.0f };
		float FilterMax{ 100.0f };
		float IdealValue{ 8.0f };
		float GaussianSigma{ 1.25f };
		std::string ScriptPath;
		bool Enabled{ true };
	};

	class WLD_API EQSAsset
	{
	public:
		std::string Name{ "TacticalQuery" };
		uint32_t Version{ 1 };
		EQSGeneratorDesc GeneratorDesc;
		std::vector<EQSTestDesc> Tests;

		EQSAsset() = default;

		bool SaveToFile(const std::string& physicalPath, std::string* error = nullptr) const;
		bool LoadFromFile(const std::string& physicalPath, std::string* error = nullptr);

		std::string ToYamlString() const;
		bool FromYamlString(const std::string& yamlStr, std::string* error = nullptr);

		std::shared_ptr<EQSQuery> BuildQuery(uint32_t targetHint = 0) const;

		// 静态缓存与加载接口
		static std::shared_ptr<EQSAsset> GetOrLoad(const std::string& logicalOrPhysicalPath);
		static void ClearCache();

	private:
		static std::unordered_map<std::string, std::shared_ptr<EQSAsset>> s_AssetCache;
	};
}
