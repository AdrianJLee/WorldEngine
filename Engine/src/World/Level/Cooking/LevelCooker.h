#pragma once

#include "World/Core/Export.h"

#include "World/Level/Authoring/LevelPackage.h"
#include "World/Level/Core/ChunkHeader.h"
#include <filesystem>
#include <string>
#include <vector>

namespace World::Level
{
	struct CookSettings
	{
		uint32_t GridCellSize = 64;
		uint32_t SubspaceID = 0;
	};

	struct CookResult
	{
		bool Success = false;
		std::string ErrorMessage;
		uint32_t ChunksCooked = 0;
		uint32_t TotalEntitiesCooked = 0;
		std::vector<std::filesystem::path> OutputChunkFiles;
	};

	class WLD_API LevelCooker
	{
	public:
		static CookResult CookPackage(
			const LevelPackage& package,
			const std::filesystem::path& outputDir,
			const CookSettings& settings = CookSettings{});
	};
}
