#include "wldpch.h"
#include "PreferencesPanel.h"

#include "../../EditorPreferences.h"
#include "World/Settings/SettingsRegistry.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"

#include <chrono>

namespace World
{
	namespace
	{
		double NowSeconds()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// 分类元数据:Group = SettingsRegistry 里的分组名(About 例外)。
		struct CategoryEntry
		{
			int Index;               // PreferencesPanel::Category 的数值
			const char* Group;
			const char* Key;         // 本地化键
			const char* English;
		};
		const CategoryEntry kCategories[] = {
			{ 0, "General",     "prefs.category.general",     "General" },
			{ 1, "Appearance",  "prefs.category.appearance",  "Appearance" },
			{ 2, "Editor",      "prefs.category.editor",      "Editor" },
			{ 3, "Workflow",    "prefs.category.workflow",    "Workflow" },
			{ 4, "Automation",  "prefs.category.automation",  "Automation" },
			{ 5, "Diagnostics", "prefs.category.diagnostics", "Diagnostics" },
			{ 6, "",            "prefs.category.about",       "About" },   // About = 只读信息页
		};

		// 该分组里是否存在偏离默认值的项(分类列表上画小圆点,用户不用逐页找)。
		bool GroupHasModified(const char* group)
		{
			if (!group || !*group)
				return false;
			for (const Settings::SettingDescriptor* descriptor :
				Settings::SettingsRegistry::Get().OfScope(Settings::SettingScope::Editor, group))
				if (descriptor->IsDefault && !descriptor->IsDefault())
					return true;
			return false;
		}
	}

	// ---- 左侧分类列表(用户 2026-09-20:"还没有类型分类") ----
	void PreferencesPanel::DrawCategoryList(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, theme.ContentBg, 0.0f });
		float y = rect.Y + 6.0f;
		for (const CategoryEntry& entry : kCategories)
		{
			const Wui::WuiRect row { rect.X + 4.0f, y, rect.W - 8.0f, 26.0f };
			const bool selected = static_cast<int>(m_Category) == entry.Index;
			if (selected)
			{
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, theme.Selection, 3.0f });
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { row.X, row.Y, 2.0f, row.H }, theme.Accent, 0.0f });
			}
			else if (ctx.IsHovered(row))
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, theme.HoverBg, 3.0f });
			const Wui::LocalizedLabel localized = Wui::TrLabel(entry.Key, entry.English);
			const std::string& label = localized.Text;
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { row.X + 12.0f, row.Y + 5.0f, 0, 0 },
				selected ? theme.Text : theme.TextMuted, 0, 1.0f, label, 13.0f, false });
			float termX = row.X + 12.0f + ctx.MeasureTextWidth(label, 13.0f) + 6.0f;
			if (!localized.Term.empty())
			{
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { termX, row.Y + 6.0f, 0, 0 },
					theme.TextMuted, 0, 1.0f, localized.Term, 11.0f, false });
				termX += ctx.MeasureTextWidth(localized.Term, 11.0f) + 6.0f;
			}
			// 改过的分组画一个小圆点(未保存/已修改的可见性)。
			if (GroupHasModified(entry.Group))
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { termX, row.Y + 11.0f, 5.0f, 5.0f }, theme.Accent, 2.5f });
			// 分类也是可点控件:登记无障碍节点(AI 通道/自动化能按 id 切页)。
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId(("prefs.category." + std::string(entry.English)).c_str());
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "prefs-category";
			node.Label = label;
			node.Value = selected ? "active" : "inactive";
			node.Rect = row;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			if (ctx.IsClicked(row))
				m_Category = static_cast<Category>(entry.Index);
			y += 28.0f;
		}
	}

	void PreferencesPanel::DrawAbout(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		const float x = rect.X;
		float y = rect.Y;
		Wui::Label(ctx, { x, y }, Wui::Tr("prefs.path", "Preferences file"), theme.TextMuted, 12.0f);
		Wui::Label(ctx, { x, y + 20.0f }, Editor::EditorPreferences::Get().Path().string(), theme.Text, 12.0f);
		y += 48.0f;
		Wui::Label(ctx, { x, y }, Wui::Tr("prefs.about.hint",
			"Editor preferences are user-level and are not committed with the project."),
			theme.TextMuted, 12.0f);
		y += 24.0f;
		Wui::Label(ctx, { x, y }, Wui::Tr("prefs.about.environment",
			"Environment variables (WLD_LANG / WLD_UI_THEME / WLD_UI_SCALE / WLD_UI_TERM_HINTS) override this file."),
			theme.TextMuted, 12.0f);
	}

	void PreferencesPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const float sidebarWidth = 170.0f;
		const Wui::WuiRect sidebar { rect.X, rect.Y, sidebarWidth, rect.H };
		const Wui::WuiRect content { rect.X + sidebarWidth + 12.0f, rect.Y + 10.0f,
			rect.W - sidebarWidth - 24.0f, rect.H - 20.0f };

		DrawCategoryList(ctx, sidebar, theme);

		if (m_Category == Category::About)
		{
			DrawAbout(ctx, content, theme);
			return;
		}

		const CategoryEntry& entry = kCategories[static_cast<size_t>(m_Category)];
		const std::vector<std::string> groups { entry.Group };
		if (Editor::DrawSettingsPage(ctx, content, theme, Settings::SettingScope::Editor, groups,
			m_PageStates[static_cast<size_t>(m_Category)]))
		{
			m_Status = Wui::Tr("prefs.autosave", "All changes apply immediately and are saved automatically");
			m_StatusSeconds = NowSeconds();
		}

		if (!m_Status.empty() && NowSeconds() - m_StatusSeconds < 4.0)
			Wui::Label(ctx, { content.X, content.Y + content.H - 16.0f }, m_Status, theme.Success, 12.0f);
	}
}
