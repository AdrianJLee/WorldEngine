#pragma once

#include "World/Core/Export.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace World::Ai
{
	struct WLD_API WritePermission
	{
		std::string roleId;
		uint32_t stage = 0;
		std::vector<std::string> allowedGlobs; // 允许写入的相对路径 Glob (如 "design/**", "assets/**")
		std::vector<std::string> deniedGlobs;  // 显式禁止的相对 Glob (如 "src/**")
	};

	class WLD_API WriteGuard
	{
	public:
		static WriteGuard& Get();

		WriteGuard() = default;
		~WriteGuard() = default;

		// 检查路径是否允许写入
		// 1. 严格禁止写入引擎源码与第三方库 (Engine/, Editor/, Runtime/, third_party/, vendor/, plugins/)
		// 2. 严格限制在当前项目根目录下 (防路径穿越)
		// 3. 符合角色与阶段的 Glob 白名单与黑名单
		bool CanWrite(const std::string& targetPath,
			const std::string& projectRoot,
			const WritePermission* perm,
			std::string& outReason) const;

		// 记录合法的通道写入到 <projectRoot>/.we/ai/writes/<sessionId>.jsonl
		bool RecordWrite(const std::string& targetPath,
			const std::string& projectRoot,
			const std::string& roleId,
			const std::string& sessionId);

		// 判断路径是否属于引擎自身保护目录
		bool IsEngineSourceTree(const std::filesystem::path& path) const;

		// 简单 Glob 模式匹配 ("*", "**", "?")
		static bool MatchGlob(const std::string& pattern, const std::string& text);
	};
}
