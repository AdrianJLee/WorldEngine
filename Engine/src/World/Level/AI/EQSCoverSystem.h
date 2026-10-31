#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace World
{
	enum class EQSQueryMode : uint8_t
	{
		Cover = 0,    // 寻找防守掩体 (视线完全遮蔽，靠近阻挡)
		Flank = 1,    // 寻找侧翼包抄位 (与威胁形成 45~90 度攻击角，具备射击线)
		Retreat = 2   // 寻找安全撤退点 (拉大与威胁距离，规避直瞄枪线)
	};

	struct CoverBox
	{
		glm::vec3 Min { 0.0f };
		glm::vec3 Max { 0.0f };
	};

	struct EQSCoverCandidate
	{
		glm::vec3 Position { 0.0f };
		float LineOfSightOcclusion { 0.0f }; // 1.0 = 视线被完全遮挡, 0.0 = 暴露在枪线下
		float DistanceScore { 0.0f };
		float AngleScore { 0.0f };
		float TotalScore { 0.0f };
	};

	class WLD_API EQSCoverSystem
	{
	public:
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
	};
}
