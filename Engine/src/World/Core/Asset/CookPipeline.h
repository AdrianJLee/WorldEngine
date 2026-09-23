#pragma once

#include "World/Core/Asset/AssetImporter.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/Export.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace World::Asset
{
	struct CookEntryResult
	{
		std::string Path;
		bool Changed = false;
		bool Failed = false;
		std::string Error;
	};

	struct CookSummary
	{
		size_t Total = 0;
		size_t Changed = 0;
		size_t Skipped = 0;
		size_t Failed = 0;
	};

	// 增量资产烘焙:遍历内容根 → 指纹比对 → 只重处理变化项。
	// 产物写入 outputDir/cooked/<logical>,数据库为 outputDir/cook.db.json。
	class WLD_API CookPipeline
	{
	public:
		explicit CookPipeline(std::vector<std::shared_ptr<IAssetImporter>> importers);

		// manifestPath 用于解析 ContentRoot;失败条目记录在 results 中,不中断整体。
		std::vector<CookEntryResult> Cook(const ProjectManifest& manifest,
			const std::filesystem::path& manifestPath,
			const std::filesystem::path& outputDir,
			bool force,
			CookSummary* summary);

	private:
		std::vector<std::shared_ptr<IAssetImporter>> m_Importers;
	};
}
