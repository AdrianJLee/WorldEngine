#include "wldpch.h"
#include "World/Level/PhysicsPainter.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <random>
#include <cmath>

namespace World
{
	std::vector<glm::vec3> PhysicsPainter::ScatterPoints(
		const glm::vec3& center,
		float radius,
		uint32_t count,
		float minDistance,
		uint32_t seed)
	{
		std::vector<glm::vec3> points;
		if (count == 0 || radius <= 1e-4f)
			return points;

		points.reserve(count);
		std::mt19937 rng(seed);
		std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
		constexpr float k2Pi = 6.28318530718f;

		const float minDistSq = minDistance * minDistance;
		const uint32_t maxAttempts = count * 50;
		uint32_t attempts = 0;

		while (points.size() < count && attempts < maxAttempts)
		{
			++attempts;
			const float r = radius * std::sqrt(dist01(rng));
			const float theta = dist01(rng) * k2Pi;
			const glm::vec3 candidate(
				center.x + r * std::cos(theta),
				center.y,
				center.z + r * std::sin(theta)
			);

			bool valid = true;
			if (minDistance > 1e-4f)
			{
				for (const auto& existing : points)
				{
					const float dx = candidate.x - existing.x;
					const float dz = candidate.z - existing.z;
					if (dx * dx + dz * dz < minDistSq)
					{
						valid = false;
						break;
					}
				}
			}

			if (valid)
				points.push_back(candidate);
		}

		return points;
	}

	void PhysicsPainter::SettleOnTerrain(
		const std::vector<glm::vec3>& dropPoints,
		const glm::vec3& halfExtents,
		const TerrainData& terrain,
		std::vector<glm::mat4>& outSettledXforms,
		bool alignToNormal,
		bool randomYaw,
		uint32_t seed)
	{
		outSettledXforms.clear();
		outSettledXforms.reserve(dropPoints.size());

		std::mt19937 rng(seed);
		std::uniform_real_distribution<float> yawDist(0.0f, 6.28318530718f);

		for (const auto& drop : dropPoints)
		{
			// 采样地表高度与表面法线
			const float groundH = terrain.SampleHeight(drop.x, drop.z);
			const glm::vec3 normal = terrain.SampleNormal(drop.x, drop.z);

			// 底部贴地沉降 (加上半高度)
			const glm::vec3 settledPos(drop.x, groundH + halfExtents.y, drop.z);

			glm::mat4 xform = glm::translate(glm::mat4(1.0f), settledPos);

			// 可选随机 Yaw 偏航角
			if (randomYaw)
			{
				const float yaw = yawDist(rng);
				xform = glm::rotate(xform, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
			}

			// 倾斜贴合法线
			if (alignToNormal && std::abs(normal.y) < 0.999f)
			{
				glm::vec3 axis = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal));
				float angle = std::acos(std::clamp(normal.y, -1.0f, 1.0f));
				xform = glm::rotate(xform, angle, axis);
			}

			outSettledXforms.push_back(xform);
		}
	}
}
