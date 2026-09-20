#pragma once

#include "EditorPanel.h"

namespace World
{
	// P4-UX1:编辑器偏好面板(用户级,自动保存)。
	// 首批开放:界面语言、英文术语对照、主题(暗/浅/跟随系统)、界面缩放。
	// 用户 2026-09-20 反馈:"大部分功能你得能让用户使用" —— 语言切换必须在这里能点。
	class PreferencesPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "prefs"; }
		const char* Title() const override { return "Editor Preferences"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		// P4-UX2(用户反馈"还没有类型分类"):左侧分类 + 右侧内容。
		enum class Category
		{
			General = 0,
			Appearance = 1,
			About = 2,
		};
		void DrawCategoryList(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		void DrawGeneral(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		void DrawAppearance(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		void DrawAbout(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);

		Category m_Category = Category::General;
		std::string m_Status;
	};
}
