#pragma once

#include "World/Settings/SettingsRegistry.h"
#include "World/WUI/WuiWidgets.h"

#include <string>
#include <vector>

namespace World::Editor
{
	// P4-UX7:注册表驱动的设置页(方案 §U4)。
	//
	// 一页 = 一个作用域 + 一组分类名;行由 SettingsRegistry 的描述符生成:
	// 标签(带英文术语对照)/ 控件 / 单位 / 复位 / "重启生效"徽标 / 悬停说明 /
	// 无障碍节点(id = "settings.<SettingId>",脚本可按稳定 id 读写)。
	struct SettingsPageState
	{
		std::string Search;        // 搜索框内容(id/label/tooltip 子串,不区分大小写)
		bool OnlyModified = false; // 只看已修改
		float ScrollY = 0.0f;
	};

	// 画一页设置。返回 true = 本帧有值被修改(调用方显示"已自动保存"状态)。
	bool DrawSettingsPage(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme,
		Settings::SettingScope scope, const std::vector<std::string>& groups, SettingsPageState& state);

	// 该作用域是否存在被改过的项(标题栏提示点用)。
	bool SettingsScopeModified(Settings::SettingScope scope);
	// 恢复整个作用域的默认值;返回是否至少改了一项。
	bool ResetSettingsScope(Settings::SettingScope scope, std::string* error);
}
