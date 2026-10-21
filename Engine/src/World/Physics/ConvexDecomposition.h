#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace World
{
	struct WLD_API ConvexHull
	{
		std::vector<glm::vec3> Vertices;
		std::vector<uint32_t> Indices;
		glm::vec3 Center { 0.0f };
		float Volume { 0.0f };

		bool ContainsPoint(const glm::vec3& pt) const;
		bool IsConvex() const;
	};

	class WLD_API ConvexDecomposer
	{
	public:
		// 从点云构建最小包围凸包 (3D 极值包络)
		static bool ComputeHull(const std::vector<glm::vec3>& inPoints, ConvexHull& outHull);

		// 将凹面网格拆解为多凸包复合体
		static void Decompose(
			const std::vector<glm::vec3>& vertices,
			const std::vector<uint32_t>& indices,
			uint32_t maxHulls,
			float concavityThreshold,
			std::vector<ConvexHull>& outHulls);
	};
}

