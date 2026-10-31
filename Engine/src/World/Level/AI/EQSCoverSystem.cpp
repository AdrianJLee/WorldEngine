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
		return ExecuteQuery(EQSQueryMode::Cover, querierPos, threatPos, queryRadius, gridSpacing, obstacles, outBestCover, outBestScore);
	}

	bool EQSCoverSystem::ExecuteQuery(
		EQSQueryMode mode,
		const glm::vec3& querierPos,
		const glm::vec3& threatPos,
		float queryRadius,
		float gridSpacing,
		const std::vector<CoverBox>& obstacles,
		glm::vec3& outBestPos,
		float& outBestScore)
	{
		outBestScore = -1.0f;
		if (queryRadius <= 0.0f || gridSpacing <= 0.1f)
			return false;

		const int steps = static_cast<int>(queryRadius / gridSpacing);
		std::vector<EQSCoverCandidate> candidates;

		const float querierDistToThreat = glm::distance(querierPos, threatPos);
		glm::vec3 threatToQuerier = (querierDistToThreat > 1e-4f)
			? glm::normalize(querierPos - threatPos) : glm::vec3(0.0f, 0.0f, 1.0f);

		// 生成空间探测网格候选点
		for (int z = -steps; z <= steps; ++z)
		{
			for (int x = -steps; x <= steps; ++x)
			{
				const glm::vec3 pt = querierPos + glm::vec3(x * gridSpacing, 0.0f, z * gridSpacing);
				const float distToQuerier = glm::distance(pt, querierPos);
				if (distToQuerier > queryRadius)
					continue;

				// 检查该点是否在障碍物内部 (不可站立穿模)
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

				// 视线遮蔽射线检测
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
				cand.DistanceScore = 1.0f - (distToQuerier / queryRadius);

				if (mode == EQSQueryMode::Cover)
				{
					// 防守掩体：视线完全遮蔽为第一优先，同时偏好靠近查询者
					cand.TotalScore = cand.LineOfSightOcclusion * 2.5f + cand.DistanceScore;
				}
				else if (mode == EQSQueryMode::Flank)
				{
					// 侧翼包抄：与威胁形成侧向 60~90 度夹角，需要具备对威胁的直视射击线 (Occlusion == 0)
					glm::vec3 threatToCand = glm::normalize(pt - threatPos);
					float cosAngle = glm::dot(threatToQuerier, threatToCand);
					cand.AngleScore = 1.0f - std::abs(cosAngle); // 90度时最大 (cosAngle = 0)
					cand.TotalScore = cand.AngleScore * 2.0f + (1.0f - cand.LineOfSightOcclusion) * 1.5f + cand.DistanceScore * 0.5f;
				}
				else if (mode == EQSQueryMode::Retreat)
				{
					// 安全撤退：点位到威胁的距离大于查询者自身到威胁的距离，且视线处于遮蔽状态
					const float candDistToThreat = rayLen;
					float retreatGain = (candDistToThreat - querierDistToThreat) / queryRadius;
					cand.TotalScore = cand.LineOfSightOcclusion * 2.0f + retreatGain * 2.0f + cand.DistanceScore;
				}

				candidates.push_back(cand);
			}
		}

		if (candidates.empty())
			return false;

		std::sort(candidates.begin(), candidates.end(),
			[](const EQSCoverCandidate& a, const EQSCoverCandidate& b) {
				return a.TotalScore > b.TotalScore;
			});

		outBestPos = candidates.front().Position;
		outBestScore = candidates.front().TotalScore;
		return true;
	}
}
