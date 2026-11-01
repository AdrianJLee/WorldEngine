#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace World
{
	class TerrainData;
	class Scene;

	enum class EQSQueryMode : uint8_t
	{
		Cover = 0,    // 寻找防守掩体 (视线完全遮蔽，靠近阻挡)
		Flank = 1,    // 寻找侧翼包抄位 (与威胁形成 45~90 度攻击角，具备射击线)
		Retreat = 2   // 寻找安全撤退点 (拉大与威胁距离，规避直瞄枪线)
	};

	enum class EQSGeneratorMode : uint8_t
	{
		ProjectedGrid = 0,
		ConcentricRings = 1,
		HybridObstacleEdge = 2
	};

	struct WLD_API CoverBox
	{
		glm::vec3 Min { 0.0f };
		glm::vec3 Max { 0.0f };
		glm::vec3 Center { 0.0f };
		glm::vec3 HalfExtents { 0.0f };
		glm::mat3 Rotation { 1.0f };
		bool IsOriented { false };

		CoverBox() = default;
		CoverBox(const glm::vec3& minBounds, const glm::vec3& maxBounds)
			: Min(minBounds)
			, Max(maxBounds)
			, Center((minBounds + maxBounds) * 0.5f)
			, HalfExtents(glm::max(glm::vec3(0.001f), (maxBounds - minBounds) * 0.5f))
			, Rotation(1.0f)
			, IsOriented(false)
		{
		}

		static CoverBox FromOBB(const glm::vec3& center, const glm::vec3& halfExtents, const glm::vec3& eulerDegrees);
		static CoverBox FromOBB(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& orientation);

		bool ContainsPoint(const glm::vec3& worldPt, float expandRadius = 0.2f) const;
		bool IntersectRay(const glm::vec3& rayOrigin, const glm::vec3& rayDir, float maxDist, float& outHitDist) const;
		float DistanceToSurfaceXZ(const glm::vec3& worldPt) const;
	};

	struct EQSCoverCandidate
	{
		glm::vec3 Position { 0.0f };
		float LineOfSightOcclusion { 0.0f }; // 1.0 = 视线被完全遮挡, 0.0 = 暴露在枪线下
		float WallProximityScore { 0.0f };   // 掩体表面贴靠分 (0..1)
		float DistanceScore { 0.0f };
		float AngleScore { 0.0f };
		float ElevationScore { 0.0f };       // 地形高差优势分 (0..1)
		float TotalScore { 0.0f };
	};

	struct WLD_API EQSScoringWeights
	{
		float OcclusionWeight { 1.8f };      // 视线遮蔽权重 (Cover为正, Flank为负/倾向直瞄)
		float WallProximityWeight { 1.2f };  // 掩体贴墙度权重
		float DistanceWeight { 0.7f };       // 距离评分权重
		float AngleWeight { 0.0f };          // 侧翼/夹角权重
		float ElevationWeight { 0.3f };      // 制高点高程权重
		float OptimalDistance { 8.0f };      // 理想交战距离 (m)

		static EQSScoringWeights ForMode(EQSQueryMode mode, float optimalDist = 8.0f);
	};
	class WLD_API EQSCoverSystem
	{
	public:
		static EQSScoringWeights& GetMutableCustomWeights();

		static bool ResolveSceneThreatPosition(
			Scene& scene,
			uint32_t querierEntityRaw,
			uint32_t threatEntityHint,
			const glm::vec3& querierPos,
			glm::vec3& outThreatPos,
			uint32_t* outResolvedThreatEntityRaw = nullptr);

		static void GatherSceneObstacles(
			Scene& scene,
			uint32_t ignoreEntityRaw1,
			uint32_t ignoreEntityRaw2,
			std::vector<CoverBox>& outObstacles);
		static bool FindBestCover(
			const glm::vec3& querierPos,
			const glm::vec3& threatPos,
			float queryRadius,
			float gridSpacing,
			const std::vector<CoverBox>& obstacles,
			glm::vec3& outBestCover,
			float& outBestScore);

		static bool ExecuteQuery(
			EQSQueryMode mode,
			const glm::vec3& querierPos,
			const glm::vec3& threatPos,
			float queryRadius,
			float gridSpacing,
			const std::vector<CoverBox>& obstacles,
			glm::vec3& outBestPos,
			float& outBestScore);

		static bool EvaluateQueryCandidates(
			EQSQueryMode mode,
			const glm::vec3& querierPos,
			const glm::vec3& threatPos,
			float queryRadius,
			float gridSpacing,
			const std::vector<CoverBox>& obstacles,
			std::vector<EQSCoverCandidate>& outCandidates,
			glm::vec3& outBestPos,
			float& outBestScore,
			const TerrainData* terrain = nullptr,
			const glm::mat4& terrainWorld = glm::mat4(1.0f),
			uint8_t generatorMode = 0);
	};
}
