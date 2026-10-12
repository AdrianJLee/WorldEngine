#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include <entt.hpp>

namespace World
{
	class WLD_API SpriteAnimationSystem
	{
	public:
		static void Update(entt::registry& registry, Timestep ts);
	};
}

