#pragma once

#include "World/Level/Authoring/LevelPackage.h"
#include <filesystem>
#include <string>

namespace World
{
	class Scene;
}

namespace World::Level
{
	struct MigrationResult
	{
		bool Success{ false };
		std::string ErrorMessage;
		uint32_t EntitiesMigrated{ 0 };
		LevelPackage Package;

		MigrationResult() = default;
		MigrationResult(MigrationResult&&) noexcept = default;
		MigrationResult& operator=(MigrationResult&&) noexcept = default;

		MigrationResult(const MigrationResult&) = delete;
		MigrationResult& operator=(const MigrationResult&) = delete;
	};

	class LegacySceneMigrationTool
	{
	public:
		static MigrationResult DeconstructSceneToPackage(
			Scene& scene,
			const std::string& levelName,
			uint32_t gridCellSize = 64);

		static bool ExportPackageToDisk(
			const LevelPackage& package,
			const std::filesystem::path& destinationDir,
			std::string* outError = nullptr);
	};
}
