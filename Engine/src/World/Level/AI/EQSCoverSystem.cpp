#include "wldpch.h"
#include "World/Level/AI/EQSCoverSystem.h"

#include <algorithm>
#include <cmath>

namespace World
{
	namespace
	{
		bool RayIntersectsBox(const glm::vec3& origin, const glm::vec3& dir, float maxDist, const CoverBox& box)
		{
			float tmin = 0.0f;
			float tmax = maxDist;

			for (int i = 0; i < 3; ++i)
			{
				if (std::abs(dir[i]) < 1e-6f)
				{
					if (origin[i] < box.Min[i] || origin[i] > box.Max[i])
						return false;
				}
				else
				{
					const float invD = 1.0f / dir[i];
					float t0 = (box.Min[i] - origin[i]) * invD;
					float t1 = (box.Max[i] - origin[i]) * invD;
					if (t0 > t1)
						std::swap(t0, t1);
					tmin = std::max(tmin, t0);
					tmax = std::min(tmax, t1);
					if (tmax < tmin)
						return false;
				}
			}
			return true;
		}
	}

	bool EQSCoverSystem::FindBestCover(
		const glm::vec3& querierPos,
		const glm::vec3& threatPos,
		float queryRadius,
		float gridSpacing,
		const std::vector<CoverBox>& obstacles,
		glm::vec3& outBestCover,
		float& outBestScore)
	{
		outBestScore = -1.0f;
		if (queryRadius <= 0.0f || gridSpacing <= 0.1f)
			return false;

		const int steps = static_cast<int>(queryRadius / gridSpacing);
		std::vector<EQSCoverCandidate> candidates;

		// 生成空间网格候选点
		for (int z = -steps; z <= steps; ++z)
		{
			for (int x = -steps; x <= steps; ++x)
			{
				const glm::vec3 pt = querierPos + glm::vec3(x * gridSpacing, 0.0f, z * gridSpacing);
				const float distToQuerier = glm::distance(pt, querierPos);
				if (distToQuerier > queryRadius)
					continue;

				// 检查该点是否在障碍物内部 (不可站立)
				bool insideObstacle = false;
				for (const auto& box : obstacles)
				{
					if (pt.x >= box.Min.x && pt.x <= box.Max.x &&
						pt.y >= box.Min.y && pt.y <= box.Max.y &&
						pt.z >= box.Min.z && pt.z <= box.Max.z)
					{
						insideObstacle = true;
						break;
					}
				}
				if (insideObstacle)
					continue;

				// 测试从威胁点 (Threat) 到该候选点 (Candidate) 是否被掩体视线阻断
				const glm::vec3 rayDir = pt - threatPos;
				const float rayLen = glm::length(rayDir);
				if (rayLen < 0.001f)
					continue;

				bool occluded = false;
				for (const auto& box : obstacles)
				{
					if (RayIntersectsBox(threatPos, rayDir / rayLen, rayLen, box))
					{
						occluded = true;
						break;
					}
				}

				EQSCoverCandidate cand;
				cand.Position = pt;
				cand.LineOfSightOcclusion = occluded ? 1.0f : 0.0f;
				// 偏好距离自己近的掩体
				cand.DistanceScore = 1.0f - (distToQuerier / queryRadius);
				cand.TotalScore = cand.LineOfSightOcclusion * 2.0f + cand.DistanceScore;

				candidates.push_back(cand);
			}
		}

		if (candidates.empty())
			return false;

		std::sort(candidates.begin(), candidates.end(),
			[](const EQSCoverCandidate& a, const EQSCoverCandidate& b) {
				return a.TotalScore > b.TotalScore;
			});

		outBestCover = candidates.front().Position;
		outBestScore = candidates.front().TotalScore;
		return true;
	}
}
