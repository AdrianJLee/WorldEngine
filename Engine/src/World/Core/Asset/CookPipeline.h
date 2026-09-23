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
		// Slang-B1w:导入成功但带提示时不再丢弃 —— 与 ImportResult::Warnings 同口径。
		std::vector<std::string> Warnings;
	};

	struct CookSummary
	{
		size_t Total = 0;
		size_t Changed = 0;
		size_t Skipped = 0;
		size_t Failed = 0;
		// 带导入警告的条目数(assets)与警告条数;哪个资产带哪条提示看条目/日志。
		size_t Warnings = 0;
		size_t WarningMessages = 0;
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
