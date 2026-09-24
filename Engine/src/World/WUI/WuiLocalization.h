#pragma once

#include "World/Core/Export.h"

#include <cstdint>
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
	//  - 缺键会被记账(`MissingLocalizationKeys()`),供"漏翻审计"脚本使用。
	//
	// 分层目录(S1,2026-09-24):语言包按**层**注册(engine / editor / project / Mod… 数量不限),
	// 每层 = `<directory>/<language>/**/*.json`(**递归**扫描;子目录 = 域/所有权单元,
	// 如 `editor/panels/settings.json`、`engine/schema/schema.json`;新增面板/域 = 放一个文件)。
	//  - 层按 `priority` 升序解析,同优先级按注册顺序;**后解析者覆盖先解析者**(高优先级胜)。
	//  - 跨层同键 = 正常覆盖(不进冲突表);**层内**(含单文件内)重复键 = 先出现者生效
	//    (序 = 相对 `<language>` 的路径,POSIX 分隔符逐字节;深浅子目录同规则),
	//    后出现的记入 `LocalizationConflicts()`(条目 `层/相对路径.json:key`,路径 = 被忽略的那次定义)。
	//  - 条目值接受 `"key": "文本"` 或 `"key": { "text": "…", ... }`(其它字段忽略,前向兼容);
	//    `$` 前缀键 = 文件元数据(`$format/$layer/$language/$owns`…),不作文案。
	//  - 命中来源:`LocalizationSource("settings.group.rendering")` → `"editor/panels/settings.json"`。
	//  - 改完语言包调 `ReloadLocalization()` 重扫(Generation++);`SetLanguage` 换语言时也会重扫。
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
	// 兼容壳(S1):= `ClearLocalizationLayers()` + 注册单层(name = "default", priority = 0),
	// 即"只有一个目录"的旧语义;多层请直接用 `RegisterLocalizationLayer`。
	WLD_API void SetLocalizationDirectory(const std::filesystem::path& directory);
	// 第一层(注册顺序)的目录;没有注册层时返回默认目录(见上)。
	WLD_API const std::filesystem::path& GetLocalizationDirectory();
	WLD_API void SetLanguage(const std::string& code);
	WLD_API const std::string& GetLanguage();
	WLD_API uint32_t LocalizationGeneration();
	WLD_API std::vector<std::string> MissingLocalizationKeys();

	// 分层语言包(S1):注册一层 = {名字, 目录, 优先级};目录下 `<language>/**/*.json` 递归自动扫描。
	// 追加注册不影响已注册层;层集合改变后下一处 `Tr`(或 `ReloadLocalization`)重扫全部层。
	WLD_API void RegisterLocalizationLayer(std::string_view name, const std::filesystem::path& directory, int priority);
	WLD_API void ClearLocalizationLayers();
	// 重扫全部层(改盘上的语言包后调用);`Generation++`,缺键/冲突记账复位。
	WLD_API void ReloadLocalization();
	// 命中该 key 的层/文件(形如 `"engine/schema/schema.json"`);未命中 = 空串。
	WLD_API std::string LocalizationSource(std::string_view key);
	// 层内重复键记账(先出现者生效)。条目形如 `"editor/shell/menu.json:menu.file"`。
	WLD_API std::vector<std::string> LocalizationConflicts();
}
