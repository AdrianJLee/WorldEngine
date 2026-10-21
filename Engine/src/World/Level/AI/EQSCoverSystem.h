#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <vector>

namespace World
{
	struct CoverBox
	{
		glm::vec3 Min { 0.0f };
		glm::vec3 Max { 0.0f };
	};

	struct EQSCoverCandidate
	{
		glm::vec3 Position { 0.0f };
		float LineOfSightOcclusion { 0.0f }; // 1.0 = 视线被完全遮挡 (优质掩体), 0.0 = 暴露在枪线下
		float DistanceScore { 0.0f };
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
	};
}
