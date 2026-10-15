#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace World::Ai
{
	struct WLD_API PackageConfig
	{
		std::string projectRoot;
		std::string profile = "debug"; // "debug" | "shipping"
		std::string outputDir;
	};

	struct WLD_API PackageResult
	{
		bool success = true;
		std::string profile;
		std::string outputDirectory;
		std::string executablePath;
		size_t artifactsCount = 0;
		uint64_t totalSizeBytes = 0;
		std::vector<std::string> errors;
		std::vector<std::string> warnings;

		std::string ToJson() const;
	};

	class WLD_API ProjectPackager
	{
	public:
		static PackageResult Package(const PackageConfig& config);
		static PackageConfig ParseConfig(const std::map<std::string, std::string>& args);
		static void RegisterCommands(class CommandRegistry& reg);
	};
}
