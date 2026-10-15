#pragma once

#include <string>

namespace World::Ai
{
	class CommandRegistry;
}

namespace World::Assistant
{
	class AssistantEngine
	{
	public:
		static std::string Chat(const std::string& message, const std::string& roleId);
		static void RegisterCommands(World::Ai::CommandRegistry& reg);
	};
}
