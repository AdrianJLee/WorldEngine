#include "wldpch.h"
#include "EditorStartup.h"

namespace World::Editor
{
	namespace
	{
		std::string& StartupSceneStorage()
		{
			static std::string path;
			return path;
		}
	}

	void SetStartupScenePath(const std::string& path)
	{
		StartupSceneStorage() = path;
	}

	const std::string& StartupScenePath()
	{
		return StartupSceneStorage();
	}

	int& AiControlPortStorage()
	{
		static int port = 0;
		return port;
	}

	void SetAiControlPort(int port)
	{
		AiControlPortStorage() = port;
	}

	int AiControlPort()
	{
		return AiControlPortStorage();
	}
}
