#pragma once

#include "World/Core/Export.h"
#include "World/Terrain/Terrain.h"
#include <glm/glm.hpp>
#include <vector>

namespace World
{
	class WLD_API PhysicsPainter
	{
	public:
		static void SettleOnTerrain(
			const std::vector<glm::vec3>& dropPoints,
			const glm::vec3& halfExtents,
			const TerrainData& terrain,
			std::vector<glm::mat4>& outSettledXforms);
	};
}
