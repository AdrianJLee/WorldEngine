#include "wldpch.h"
#include "World/Level/Authoring/LevelCatalog.h"

#include "World/Core/Log.h"

#include <algorithm>

namespace World::Level
{
	std::filesystem::path LevelCatalog::EntryPathFor(const std::filesystem::path& contentRoot,
		const std::string& id)
	{
		return contentRoot / LevelPackage::ScenesDirName / (id + LevelPackage::EntryExtension);
	}

	// 成对校验:入口文件里的 Id 必须等于文件名 stem,且内容目录必须存在。
	// 失败原因写进 *reason(便于日志与编辑器报错)。
	static bool ValidatePairing(const std::filesystem::path& entryPath, const std::string& id,
		std::filesystem::path* outContentDir, std::string* reason)
	{
		const std::string stem = entryPath.stem().string();
		if (stem != id)
		{
			if (reason) *reason = "entry file name '" + stem + "' does not match level id '" + id + "'";
			return false;
		}

		const std::filesystem::path contentRoot = entryPath.parent_path().parent_path();
		const std::filesystem::path contentDir = contentRoot / LevelPackage::LevelsDirName / id;
		std::error_code ec;
		if (!std::filesystem::is_directory(contentDir, ec))
		{
			if (reason) *reason = "content directory missing: " + contentDir.u8string();
			return false;
		}

		if (outContentDir) *outContentDir = contentDir;
		return true;
	}

	std::vector<LevelInfo> LevelCatalog::Discover(const std::filesystem::path& contentRoot)
	{
		std::vector<LevelInfo> levels;
		if (contentRoot.empty())
			return levels;

		const std::filesystem::path scenesDir = contentRoot / LevelPackage::ScenesDirName;
		std::error_code ec;
		if (!std::filesystem::is_directory(scenesDir, ec))
			return levels;

		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(scenesDir, ec))
		{
			if (ec)
				break;
			std::error_code fileError;
			if (!entry.is_regular_file(fileError)
				|| entry.path().extension() != LevelPackage::EntryExtension)
				continue;

			LevelPackage package;
			std::string loadError;
			if (!package.Load(entry.path(), &loadError))
			{
				WLD_CORE_WARN("LevelCatalog: 跳过 '{0}': {1}", entry.path().u8string(), loadError);
				continue;
			}

			LevelInfo info;
			info.Id = package.GetId();
			info.EntryPath = entry.path();
			if (!ValidatePairing(entry.path(), info.Id, &info.ContentDir, &loadError))
			{
				WLD_CORE_WARN("LevelCatalog: 跳过 '{0}': {1}", entry.path().u8string(), loadError);
				continue;
			}

			info.DisplayName = package.GetName().empty() ? info.Id : package.GetName();
			info.DataLayers = package.GetDataLayers();
			levels.push_back(std::move(info));
		}

		std::sort(levels.begin(), levels.end(),
			[](const LevelInfo& a, const LevelInfo& b) { return a.Id < b.Id; });
		return levels;
	}

	const LevelInfo* LevelCatalog::Find(const std::vector<LevelInfo>& levels, const std::string& id)
	{
		for (const LevelInfo& level : levels)
			if (level.Id == id)
				return &level;
		return nullptr;
	}

	bool LevelCatalog::Resolve(const std::filesystem::path& contentRoot, const std::string& id,
		LevelInfo* out, std::string* error)
	{
		if (id.empty())
		{
			if (error) *error = "level id is empty";
			return false;
		}
		if (id.find_first_of("/\\") != std::string::npos || id == "." || id == "..")
		{
			if (error) *error = "invalid level id '" + id + "'";
			return false;
		}

		const std::filesystem::path entryPath = EntryPathFor(contentRoot, id);
		std::error_code ec;
		if (!std::filesystem::is_regular_file(entryPath, ec))
		{
			if (error) *error = "level '" + id + "' not found: " + entryPath.u8string();
			return false;
		}

		LevelPackage package;
		std::string loadError;
		if (!package.Load(entryPath, &loadError))
		{
			if (error) *error = "level '" + id + "' rejected: " + loadError;
			return false;
		}

		LevelInfo info;
		info.Id = package.GetId();
		info.EntryPath = entryPath;
		if (!ValidatePairing(entryPath, info.Id, &info.ContentDir, &loadError))
		{
			if (error) *error = "level '" + id + "' rejected: " + loadError;
			return false;
		}
		info.DisplayName = package.GetName().empty() ? info.Id : package.GetName();
		info.DataLayers = package.GetDataLayers();

		if (out) *out = std::move(info);
		return true;
	}
}
