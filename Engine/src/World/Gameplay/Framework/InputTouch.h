#pragma once

#include "World/Core/Export.h"
#include <glm/glm.hpp>
#include <cstdint>

namespace World::Gameplay
{
	enum class TouchPhase : uint8_t
	{
		Began = 0,
		Moved,
		Stationary,
		Ended,
		Canceled
	};

	struct TouchPoint
	{
		uint32_t FingerId = 0;
		glm::vec2 Position{ 0.0f };
		glm::vec2 Delta{ 0.0f };
		float Pressure = 1.0f;
		TouchPhase Phase = TouchPhase::Began;
	};
}
