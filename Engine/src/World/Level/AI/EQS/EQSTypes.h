#pragma once

#include "World/Core/Export.h"
#include "World/Level/AI/EQSCoverSystem.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace World
{
	class Scene;
	class TerrainData;
}

namespace World::EQS
{
	enum class TestPurpose : uint8_t
	{
		ScoreOnly = 0,      // 仅加权打分，不淘汰
		FilterOnly = 1,     // 仅硬过滤（不符合阈值直接淘汰）
		FilterAndScore = 2  // 先硬过滤淘汰，合格者再加权打分
	};

	enum class ScoringCurve : uint8_t
	{
		Linear = 0,         // 线性递增 (越大越好)
		Inverse = 1,        // 反向递减 (越小/越近越好)
		ClampedGaussian = 2,// 高斯钟形曲线 (中心理想值拿满分，两端衰减，用于最佳交战/贴墙距离)
		StepThreshold = 3   // 阶跃阈值
	};

	struct WLD_API EQSCandidateItem
	{
		glm::vec3 Position { 0.0f };
		float TotalScore { 0.0f };
		bool FilteredOut { false };
		std::vector<float> TestScores; // 保留每个 Test 的单项得分，专供视口诊断与雷达卡片
	};

	struct WLD_API EQSQueryContext
	{
		glm::vec3 QuerierPos { 0.0f };
		uint32_t QuerierEntityRaw { 0 };
		glm::vec3 ThreatPos { 0.0f };
		uint32_t ThreatEntityHint { 0 };
		float QueryRadius { 20.0f };
		float GridSpacing { 2.0f };
		float StandingEyeHeight { 1.6f };
		float CrouchingEyeHeight { 0.9f };
		float AgentRadius { 0.35f };
		const Scene* ScenePtr { nullptr };
		const TerrainData* Terrain { nullptr };
		glm::mat4 TerrainWorldTransform { 1.0f };
		std::vector<World::CoverBox> Obstacles;
	};
}
