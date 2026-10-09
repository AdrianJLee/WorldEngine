#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace World::Level
{
	enum class ChunkState : uint8_t
	{
		Unloaded = 0,
		IO_Loading = 1,
		MemoryReady = 2,
		Registering = 3,
		Active = 4,
		Unregistering = 5
	};

	using SubspaceID = uint32_t;
	constexpr SubspaceID MainWorldSubspace = 0;

	struct ChunkBounds
	{
		glm::vec3 Min{ 0.0f };
		glm::vec3 Max{ 0.0f };

		bool Contains(const glm::vec3& point) const
		{
			return point.x >= Min.x && point.x <= Max.x &&
			       point.y >= Min.y && point.y <= Max.y &&
			       point.z >= Min.z && point.z <= Max.z;
		}
	};
}
