#pragma once

#include "World/Core/Export.h"
#include "World/Terrain/Terrain.h"
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace World
{
	class WLD_API PhysicsPainter
	{
	public:
		// 在给定中心与半径内生成随机散布点 (基于拒绝采样，满足最小间距约束)
		static std::vector<glm::vec3> ScatterPoints(
			const glm::vec3& center,
			float radius,
			uint32_t count,
			float minDistance = 0.5f,
			uint32_t seed = 1337);

		// 计算降落点在地表的垂直沉降位置与倾斜旋转矩阵 (贴合地表法线与可选随机 Yaw 偏航角)
		static void SettleOnTerrain(
			const std::vector<glm::vec3>& dropPoints,
			const glm::vec3& halfExtents,
			const TerrainData& terrain,
			std::vector<glm::mat4>& outSettledXforms,
			bool alignToNormal = true,
			bool randomYaw = false,
			uint32_t seed = 42);
	};
}
