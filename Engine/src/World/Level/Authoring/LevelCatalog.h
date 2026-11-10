#pragma once

#include "World/Core/Export.h"
#include "World/Level/Authoring/LevelPackage.h"

#include <filesystem>
#include <string>
#include <vector>

namespace World::Level
{
	// 一个可打开的关卡。
	//
	// 关卡入口 = 内容根下 `scenes/<Id>.wlevel`(存 manifest,可在内容浏览器里双击打开);
	// 内容 = 同内容根的 `levels/<Id>/entries/*.entity`。二者按 **Id 同名**成对,
	// 由 `LevelCatalog` 校验:入口文件名必须等于 manifest 里的 `Id`、且内容目录存在,
	// 不一致即**报错**而不是静默降级 —— 同名耦合靠约定不如靠校验。
	struct LevelInfo
	{
		std::string Id;
		std::string DisplayName;
		std::filesystem::path EntryPath;     // 可打开的入口文件(`scenes/<Id>.wlevel`)
		std::filesystem::path ContentDir;    // `levels/<Id>/`
		std::vector<DataLayerDefinition> DataLayers;
	};

	// 关卡目录的发现与解析。只读文件系统,不碰 Scene/WorldContext。
	class WLD_API LevelCatalog
	{
	public:
		// 扫描 `<contentRoot>/scenes/*.wlevel`,按 Id 排序返回。目录不存在 = 空表(不是错误)。
		// 不成对(入口名 ≠ Id、或内容目录缺失)的关卡**跳过并告警**,不拖垮其它关卡。
		static std::vector<LevelInfo> Discover(const std::filesystem::path& contentRoot);

		static const LevelInfo* Find(const std::vector<LevelInfo>& levels, const std::string& id);
		// 入口文件路径。`id` 必须是不含分隔符的裸名(拒绝路径穿越)。
		static std::filesystem::path EntryPathFor(const std::filesystem::path& contentRoot,
			const std::string& id);

		// 定位单个关卡并做成对校验。失败填 error 并返回 false。
		static bool Resolve(const std::filesystem::path& contentRoot, const std::string& id,
			LevelInfo* out, std::string* error = nullptr);
	};
}
