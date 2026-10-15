#pragma once

#include "StyleLock.h"
#include "BudgetGuard.h"
#include <string>

namespace World::Ai
{
	class CommandRegistry;
}

namespace World::Gen
{
	struct GenRequest
	{
		std::string type = "2d_sprite"; // 2d_sprite | texture | 3d_prop
		std::string prompt;
		std::string targetPath; // 项目相对路径 (如 "assets/textures/hero.png")
		double estimatedCost = 0.05;
		std::string provider = "mock";
	};

	struct GenResult
	{
		bool success = true;
		std::string outputPath;
		std::string provenanceSidecarPath;
		std::string injectedPrompt;
		double costIncurred = 0.0;
		std::string error;

		std::string ToJson() const;
	};

	class GenPipeline
	{
	public:
		static GenResult Generate(const std::string& projectRoot, const GenRequest& request);
		static void RegisterCommands(World::Ai::CommandRegistry& reg);
	};
}
