#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace World::Level::AI
{
	enum class AiLodTier : uint8_t
	{
		Tier1_Active = 0,
		Tier2_Hibernated = 1,
		Tier3_Virtual = 2
	};

	struct AiVirtualSnapshot
	{
		uint64_t EntityUUID{ 0 };
		uint64_t HomeChunkID{ 0 };
		glm::vec3 Position{ 0.0f };
		float Health{ 100.0f };
		uint32_t CurrentWaypointIndex{ 0 };
		uint8_t AlertLevel{ 0 };
	};
}
