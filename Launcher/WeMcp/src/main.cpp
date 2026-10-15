#include "McpServer.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
	uint16_t remotePort = 0;
	std::string projectDir;
	std::string initialRole;

	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg.rfind("--port=", 0) == 0)
		{
			try { remotePort = static_cast<uint16_t>(std::stoi(arg.substr(7))); }
			catch (...) {}
		}
		else if (arg.rfind("--project=", 0) == 0)
		{
			projectDir = arg.substr(10);
		}
		else if (arg.rfind("--role=", 0) == 0)
		{
			initialRole = arg.substr(7);
		}
	}

	auto bridge = std::make_shared<World::Mcp::EngineBridge>();
	bridge->Initialize(remotePort, projectDir);

	World::Mcp::McpServer server;
	if (!initialRole.empty())
	{
		server.SetCurrentRole(initialRole);
	}
	server.SetEngineBridge(bridge);

	server.RunStdioLoop(std::cin, std::cout);
	std::quick_exit(0);
	return 0;
}
