#pragma once

#include "World/Core/Export.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace World::Wui
{
	// P4-UX1:本地化框架(预留 + 可直接使用)。
	//
	// 口径:
	//  - **源码内联文案 = 默认语言(简体中文)**;目录文件只提供其它语言的覆盖,
	//    因此"没翻译"永远显示可读文本,不会出现裸 key。
	//  - 用法:`Wui::Tr("settings.group.rendering", "渲染 (project.we.yaml → rendering:)")`。
	//  - 语言来源:`WLD_LANG`(zh-CN 默认)或 `SetLanguage`;默认语言下不查表。
	//  - 目录路径:`<GAME_DIR>assets/localization/<lang>.json`,形如
	//    `{ "settings.group.rendering": "Rendering", ... }`;首个 Tr 调用时惰性加载。
	//  - 缺键会被记账(`MissingKeys()`),供 U5 的"漏翻审计"脚本使用。
	WLD_API std::string Tr(std::string_view key, std::string_view fallback);
	WLD_API bool LoadLocalizationCatalog(const std::filesystem::path& path);
	WLD_API void SetLanguage(const std::string& code);
	WLD_API const std::string& GetLanguage();
	WLD_API uint32_t LocalizationGeneration();
	WLD_API std::vector<std::string> MissingLocalizationKeys();
}

