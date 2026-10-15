#pragma once

#include "StudioPolicy.h"
#include <string>
#include <vector>

namespace World::Ai
{
	class CommandRegistry;
}

namespace World::Studio
{
	struct StudioValidationResult
	{
		bool valid = true;
		std::vector<std::string> errors;
		std::vector<std::string> warnings;

		std::string ToJson() const;
	};

	class StudioValidator
	{
	public:
		static StudioValidationResult Validate(const StudioPolicy& policy);
		static void RegisterCommands(World::Ai::CommandRegistry& reg);
	};
}
