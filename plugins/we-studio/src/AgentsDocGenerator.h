#pragma once

#include "StudioPolicy.h"
#include <string>

namespace World::Studio
{
	class AgentsDocGenerator
	{
	public:
		static std::string Generate(const StudioPolicy& policy);
		static bool WriteToProject(const std::string& projectRoot, const StudioPolicy& policy);
	};
}
