#pragma once

#include "World/Core/Export.h"
#include "World/Level/AI/EQS/EQSTypes.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace World::EQS
{
	class WLD_API IEQSGenerator
	{
	public:
		virtual ~IEQSGenerator() = default;
		virtual std::string GetName() const = 0;
		virtual void GenerateItems(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& outItems) = 0;
	};

	class WLD_API EQSGeneratorRegistry
	{
	public:
		static EQSGeneratorRegistry& Get();
		void Register(const std::string& name, std::shared_ptr<IEQSGenerator> generator);
		std::shared_ptr<IEQSGenerator> Find(const std::string& name) const;

		template<typename T, typename... Args>
		void RegisterGenerator(const std::string& name, Args&&... args)
		{
			Register(name, std::make_shared<T>(std::forward<Args>(args)...));
		}

	private:
		EQSGeneratorRegistry();
		std::unordered_map<std::string, std::shared_ptr<IEQSGenerator>> m_Generators;
	};

	// 1. EQSGenerator_ProjectedGrid (网格采样 + 地形贴合)
	class WLD_API EQSGenerator_ProjectedGrid : public IEQSGenerator
	{
	public:
		float Radius { 0.0f }; // 0 = 使用 ctx.QueryRadius
		float Spacing { 0.0f }; // 0 = 使用 ctx.GridSpacing
		bool ProjectToTerrain { true };

		EQSGenerator_ProjectedGrid() = default;
		EQSGenerator_ProjectedGrid(float radius, float spacing, bool projectTerrain = true)
			: Radius(radius), Spacing(spacing), ProjectToTerrain(projectTerrain) {}

		std::string GetName() const override { return "Generator_ProjectedGrid"; }
		void GenerateItems(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& outItems) override;
	};

	// 2. EQSGenerator_Circle (极坐标同心环/甜甜圈环带采样)
	class WLD_API EQSGenerator_Circle : public IEQSGenerator
	{
	public:
		float InnerRadius { 0.0f };
		float OuterRadius { 0.0f }; // 0 = 使用 ctx.QueryRadius
		float StepDistance { 0.0f }; // 0 = 使用 ctx.GridSpacing
		int MinPointsPerRing { 6 };
		bool ProjectToTerrain { true };

		EQSGenerator_Circle() = default;
		EQSGenerator_Circle(float innerRadius, float outerRadius, float stepDist = 0.0f, bool projectTerrain = true)
			: InnerRadius(innerRadius), OuterRadius(outerRadius), StepDistance(stepDist), ProjectToTerrain(projectTerrain) {}

		std::string GetName() const override { return "Generator_Circle"; }
		void GenerateItems(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& outItems) override;
	};

	// 3. EQSGenerator_AroundObstacles (障碍物四壁外扩贴墙点 + 可选基准网格)
	class WLD_API EQSGenerator_AroundObstacles : public IEQSGenerator
	{
	public:
		float WallOffset { 0.75f };
		bool IncludeGrid { true };
		bool ProjectToTerrain { true };

		EQSGenerator_AroundObstacles() = default;
		explicit EQSGenerator_AroundObstacles(float wallOffset, bool includeGrid = true, bool projectTerrain = true)
			: WallOffset(wallOffset), IncludeGrid(includeGrid), ProjectToTerrain(projectTerrain) {}

		std::string GetName() const override { return "Generator_AroundObstacles"; }
		void GenerateItems(const EQSQueryContext& ctx, std::vector<EQSCandidateItem>& outItems) override;
	};
}
