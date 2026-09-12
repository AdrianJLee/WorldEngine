#include "wldpch.h"
#include "World/Modules/GameModuleHost.h"
#include "World/Modules/ModuleManager.h"
#include "World/Core/WorldContext.h"

#include <filesystem>

namespace World::Modules
{
	bool GameModuleHost::LoadDefault(WorldContext& context, std::string* error)
	{
		std::string dllPath;
		char exePathBuf[MAX_PATH];
		if (GetModuleFileNameA(NULL, exePathBuf, MAX_PATH))
		{
			const std::filesystem::path exeDir = std::filesystem::path(exePathBuf).parent_path();
			const std::filesystem::path binDir = exeDir / "bin";
			if (std::filesystem::exists(binDir))
			{
				for (const auto& entry : std::filesystem::recursive_directory_iterator(binDir))
				{
					if (entry.is_regular_file() && entry.path().filename() == "Game.dll")
					{
						dllPath = entry.path().string();
						break;
					}
				}
			}
		}
		if (dllPath.empty())
			dllPath = std::string(WLD_OUTPUT_DIR) + "bin/" + WLD_BUILD_TYPE + "/Game/" + WLD_BUILD_TYPE + "/Game.dll";

		return context.Modules().Load(dllPath, context, error) == ModuleManager::Status::Ok;
	}
}
