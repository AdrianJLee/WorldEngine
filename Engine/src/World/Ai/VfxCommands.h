#pragma once

#include "World/Core/Export.h"
#include "World/Ai/CommandRegistry.h"

namespace World::Ai
{
	class CommandRegistry;

	class WLD_API VfxCommands
	{
	public:
		static void RegisterCommands(CommandRegistry& reg);
	};
}
