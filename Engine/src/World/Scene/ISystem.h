#pragma once

#include "World/Core/Export.h"
#include "World/Core/Timestep.h"
#include "World/Gameplay/SystemRegistry.h"
#include <string_view>
#include <string>
#include <vector>

namespace World
{
	class Scene;

	class WLD_API ISystem
	{
	public:
		virtual ~ISystem() = default;
		virtual std::string_view Name() const = 0;
		virtual Gameplay::SystemPhase Phase() const { return Gameplay::SystemPhase::Update; }
		virtual bool ParallelSafe() const { return false; }
		virtual std::vector<std::string> After() const { return {}; }
		virtual void Update(Scene& scene, Timestep dt) = 0;
	};
}
