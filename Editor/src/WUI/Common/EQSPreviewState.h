#pragma once

#include "World/Scene/Entity.h"
#include "World/Scene/Components.h"
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace World
{
	class Scene;

	struct EQSTestItem
	{
		uint8_t Type = 0;        // 0: Distance, 1: Trace, 2: Dot, 3: WallProximity, 4: Height, 5: Overlap
		uint8_t Context = 0;     // 0: Querier, 1: Target, 2: AllThreats, 3: Custom
		uint8_t Curve = 0;       // 0: Linear, 1: Inverse, 2: Gaussian
		float Weight = 1.0f;
		float IdealValue = 8.0f;
		std::string Name;
	};

	struct EQSCandidatePreview
	{
		glm::vec3 Position { 0.0f };
		float TotalScore { 0.0f };
		bool Occluded { false };
		bool FilteredOut { false };
	};

	struct EQSPreviewState
	{
		bool Evaluated = false;
		entt::entity TargetEntity = entt::null;
		glm::vec3 QuerierPos { 0.0f };
		glm::vec3 ThreatPos { 15.0f, 2.0f, 15.0f };
		glm::vec3 BestCoverPos { 0.0f };
		float BestScore { 0.0f };
		std::vector<EQSCandidatePreview> Candidates;

		// 诊断指标
		size_t TotalSampleCount = 0;
		size_t QualifiedCount = 0;
		size_t HardFilteredCount = 0;

		// 流水线测试项栈与配置
		std::vector<EQSTestItem> PipelineTests;
		uint8_t GeneratorMode = 0; // 0: Projected Grid, 1: Concentric Circle, 2: Around Obstacles
		float QueryRadius = 20.0f;
		float GridSpacing = 2.0f;
		float RingCount = 5.0f;
		float WallOffset = 0.75f;
		uint32_t ThreatEntityHint = 0;
		uint8_t QueryMode = 0; // 0: Cover, 1: Flank, 2: Retreat, 3: Sniper

		// 战术核心权重微调
		float OcclusionWeight = 1.8f;
		float WallProximityWeight = 1.2f;
		float OptimalDistance = 8.0f;
		float ElevationWeight = 0.3f;

		std::string LastStatus;
	};

	extern EQSPreviewState g_EQSPreviewState;

	void LoadEQSPreset(uint8_t mode, EQSComponent* comp, EQSPreviewState& state);
	bool ExecuteEQSPipeline(Scene* scene, Entity entity, EQSComponent* comp, EQSPreviewState& state);
}
