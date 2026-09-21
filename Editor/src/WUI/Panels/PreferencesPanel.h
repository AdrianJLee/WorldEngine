#pragma once

#include "EditorPanel.h"
#include "../SettingsUi.h"

#include <array>
#include <string>

namespace World
{
	// P4-UX1:编辑器偏好面板(用户级,自动保存)。
	// P4-UX7 起:页面由 `SettingsRegistry` 驱动(见 EditorPreferences::RegisterSettings),
	// 本面板只负责分类导航 + 整页渲染,新增偏好项不需要再改这里。
	// 用户 2026-09-20 反馈:"大部分功能你得能让用户使用" —— 语言切换/字号/热重载/诊断都得能点。
	class PreferencesPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "prefs"; }
		const char* Title() const override { return "Editor Preferences"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		// P4-UX2(用户反馈"还没有类型分类"):左侧分类 + 右侧内容。
		// 分类名 = SettingsRegistry 的 Group(About 例外,是只读信息页)。
		enum class Category
		{
			General = 0,
			Appearance = 1,
			Editor = 2,
			Workflow = 3,
			Automation = 4,
			Diagnostics = 5,
			About = 6,
			Count = 7,
		};
		void DrawCategoryList(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		void DrawAbout(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);

		Category m_Category = Category::General;
		// 每页一份搜索/只看已修改/滚动状态(切页不互相污染)。
		std::array<Editor::SettingsPageState, static_cast<size_t>(Category::Count)> m_PageStates;
		std::string m_Status;
		double m_StatusSeconds = 0.0;
	};
}
