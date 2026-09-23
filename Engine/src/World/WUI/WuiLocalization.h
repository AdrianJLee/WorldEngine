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
	//  - **源码内联文案 = 默认语言(English)**;目录文件只提供其它语言的覆盖,
	//    因此"没翻译"永远显示可读的英文,不会出现裸 key。
	//  - 用法:`Wui::Tr("settings.group.rendering", "Rendering (project.we.yaml → rendering:)")`。
	//  - 语言来源:`WLD_LANG`(默认 `en`)或 `SetLanguage`;默认语言下不查表。
	//  - 目录路径:`<GAME_DIR>assets/localization/<lang>.json`,形如
	//    `{ "settings.group.rendering": "渲染 (project.we.yaml → rendering:)", ... }`;
	//    首个 Tr 调用时惰性加载。
	//  - 缺键会被记账(`MissingKeys()`),供 U5 的"漏翻审计"脚本使用。
	WLD_API std::string Tr(std::string_view key, std::string_view fallback);

	// 术语对照(P4-UX1,用户要求):中文界面下把英文术语以 Caption/次要色画在主标签之后,
	// 有歧义时可直接对照;英文界面下 Term 为空(不重复画)。
	struct LocalizedLabel
	{
		std::string Text;   // 当前语言的主文案
		std::string Term;   // 英文术语(仅在需要对照时非空)
	};
	WLD_API LocalizedLabel TrLabel(std::string_view key, std::string_view englishTerm);
	// 是否显示英文术语对照(默认开;偏好/环境变量 `WLD_UI_TERM_HINTS=0` 关闭)。
	WLD_API bool ShowTermHints();
	WLD_API void SetShowTermHints(bool enabled);

	WLD_API bool LoadLocalizationCatalog(const std::filesystem::path& path);
	// 目录所在文件夹:编辑器用 `<EDITOR_DIR>assets/localization`(编辑器自己的语言包),
	// 游戏内容用 `<GAME_DIR>assets/localization`(Runtime/打包产物)。默认后者。
	WLD_API void SetLocalizationDirectory(const std::filesystem::path& directory);
	WLD_API const std::filesystem::path& GetLocalizationDirectory();
	WLD_API void SetLanguage(const std::string& code);
	WLD_API const std::string& GetLanguage();
	WLD_API uint32_t LocalizationGeneration();
	WLD_API std::vector<std::string> MissingLocalizationKeys();
}
