#pragma once

#include "World/Core/Export.h"
#include "World/Level/Authoring/LevelPackage.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World::Level
{
	// 一个可加载的关卡(Level 2.0 形态)。
	//
	// 关卡 = 内容根下 `levels/<Id>/` 里的一个关卡包目录;`Id` 就是目录名,显示名取
	// manifest 的 `Name`(空则回落 Id)。**不再有 `levels.welevel` 清单文件** ——
	// 关卡集合由目录结构本身表达,加一个关卡 = 加一个目录,少一份会漂移的登记表。
	struct LevelInfo
	{
		std::string Id;
		std::string DisplayName;
		std::filesystem::path PackageDir;
		std::vector<DataLayerDefinition> DataLayers;
	};

	// 关卡目录的发现与解析。只读文件系统,不碰 Scene/WorldContext。
	class WLD_API LevelCatalog
	{
	public:
		// 扫描 `<contentRoot>/levels/*/manifest.yaml`,按 Id 排序返回。
		// 目录不存在 = 没有关卡(空表,不是错误)。
		static std::vector<LevelInfo> Discover(const std::filesystem::path& contentRoot);

		static const LevelInfo* Find(const std::vector<LevelInfo>& levels, const std::string& id);

		// 定位单个关卡。失败填 error 并返回 false。
		static bool Resolve(const std::filesystem::path& contentRoot, const std::string& id,
			LevelInfo* out, std::string* error = nullptr);
	};
}
