#pragma once

#include "World/Core/Export.h"
#include "World/Scene/Components.h"

#include <cstdint>
#include <string>

namespace World
{
	struct WLD_API PhysicsMaterial
	{
		std::string Id = "default";
		float Friction = 0.5f;
		float Restitution = 0.0f;
		PhysMatCombineMode FrictionCombine = PhysMatCombineMode::Average;
		PhysMatCombineMode RestitutionCombine = PhysMatCombineMode::Average;

		static float Combine(PhysMatCombineMode mode, float a, float b);

		bool SaveToFile(const std::string& filepath) const;
		static bool LoadFromFile(const std::string& filepath, PhysicsMaterial& outMaterial);
	};
}
