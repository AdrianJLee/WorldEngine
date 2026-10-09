#include "wldpch.h"
#include "World/Level/Authoring/LevelCatalog.h"

#include <algorithm>

namespace World::Level
{
	namespace
	{
		constexpr const char* kManifestFileName = "manifest.yaml";
		constexpr const char* kLevelsDirName = "levels";
	}

	std::vector<LevelInfo> LevelCatalog::Discover(const std::filesystem::path& contentRoot)
	{
		std::vector<LevelInfo> levels;
		if (contentRoot.empty())
			return levels;

		const std::filesystem::path levelsDir = contentRoot / kLevelsDirName;
		std::error_code ec;
		if (!std::filesystem::is_directory(levelsDir, ec))
			return levels;

		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(levelsDir, ec))
		{
			if (ec)
				break;
			std::error_code entryError;
			if (!entry.is_directory(entryError))
				continue;

			LevelInfo info;
			info.Id = entry.path().filename().string();
			info.PackageDir = entry.path();

			LevelPackage package;
			std::string loadError;
			if (!package.Load(entry.path(), &loadError))
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
		// 目录名直接拼 id:先做一次"不含分隔符/上跳"的校验,避免 `--level ../../x` 这类越界。
		if (id.find_first_of("/\\") != std::string::npos || id == "." || id == "..")
		{
			if (error) *error = "invalid level id '" + id + "'";
			return false;
		}

		const std::filesystem::path packageDir = contentRoot / kLevelsDirName / id;
		std::error_code ec;
		if (!std::filesystem::is_directory(packageDir, ec))
		{
			if (error) *error = "level '" + id + "' not found under " + packageDir.u8string();
			return false;
		}

		LevelInfo info;
		info.Id = id;
		info.PackageDir = packageDir;

		LevelPackage package;
		std::string loadError;
		if (!package.Load(packageDir, &loadError))
		{
			if (error) *error = "level '" + id + "' rejected: " + loadError;
			return false;
		}
		info.DisplayName = package.GetName().empty() ? id : package.GetName();
		info.DataLayers = package.GetDataLayers();

		if (out)
			*out = std::move(info);
		return true;
	}
}
