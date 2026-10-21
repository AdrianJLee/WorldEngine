#include "wldpch.h"
#include "World/Level/PhysicsPainter.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace World
{
	void PhysicsPainter::SettleOnTerrain(
		const std::vector<glm::vec3>& dropPoints,
		const glm::vec3& halfExtents,
		const TerrainData& terrain,
		std::vector<glm::mat4>& outSettledXforms)
	{
		outSettledXforms.clear();
		outSettledXforms.reserve(dropPoints.size());

		for (const auto& drop : dropPoints)
		{
			// 采样地表高度与表面法线
			const float groundH = terrain.SampleHeight(drop.x, drop.z);
			const glm::vec3 normal = terrain.SampleNormal(drop.x, drop.z);

			// 底部贴地沉降 (加上半高度)
			const glm::vec3 settledPos(drop.x, groundH + halfExtents.y, drop.z);

			// 倾斜贴合法线
			glm::mat4 xform = glm::translate(glm::mat4(1.0f), settledPos);
			if (std::abs(normal.y) < 0.999f)
			{
				glm::vec3 axis = glm::normalize(glm::cross(glm::vec3(0, 1, 0), normal));
				float angle = std::acos(std::clamp(normal.y, -1.0f, 1.0f));
				xform = glm::rotate(xform, angle, axis);
			}
			outSettledXforms.push_back(xform);
		}
	}
}
