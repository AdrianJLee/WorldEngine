#pragma once

#include "StyleLock.h"
#include "BudgetGuard.h"
#include <memory>
#include <string>

namespace World::Ai
{
	class CommandRegistry;
}

namespace World::Gen
{
	struct GenRequest
	{
		std::string type = "2d_sprite"; // 2d_sprite | texture | 3d_prop | audio_sfx
		std::string prompt;
		std::string targetPath; // 项目相对路径 (如 "assets/textures/hero.png")
		double estimatedCost = 0.05;
		std::string provider = "mock"; // "mock" | "procedural" | "openai_image" | "cloud"
		std::string model = "we-gen-v1";
		int seed = 42001;
		std::string assetId; // 持久 AssetId，用于占位资源原地平滑替换
	};

	struct GenResult
	{
		bool success = true;
		std::string outputPath;
		std::string provenanceSidecarPath;
		std::string injectedPrompt;
		std::string assetId;
		int seed = 0;
		double costIncurred = 0.0;
		std::string error;

		std::string ToJson() const;
	};

	// 路线 A 抽象多模态生成接口 (§6.9)
	class IGenProvider
	{
	public:
		virtual ~IGenProvider() = default;
		virtual std::string GetProviderName() const = 0;
		virtual bool GenerateAsset(const GenRequest& request, const StyleLock& style, const std::string& fullOutputPath, std::string& outError) = 0;
	};

	class GenPipeline
	{
	public:
		static GenResult Generate(const std::string& projectRoot, const GenRequest& request);
		static void RegisterCommands(World::Ai::CommandRegistry& reg);
	};
}
