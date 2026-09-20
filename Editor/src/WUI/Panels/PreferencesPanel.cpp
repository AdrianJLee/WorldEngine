#include "wldpch.h"
#include "PreferencesPanel.h"

#include "../../EditorPreferences.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiWidgets.h"

namespace World
{
	namespace
	{
		struct LanguageOption { const char* Code; const char* LabelKey; const char* EnglishLabel; };
		const LanguageOption kLanguages[] = {
			{ "en", "prefs.language.en", "English" },
			{ "zh-CN", "prefs.language.zh", "简体中文" },
		};

		int LanguageIndex(const std::string& code)
		{
			for (int i = 0; i < 2; ++i)
				if (code == kLanguages[i].Code)
					return i;
			return 0;
		}

		// 一行"标签 + 控件"的标准排版(标签左、控件右、说明在更右)。
		// 排版约定:标签 x,控件 x+300(宽 180),说明文字 x+500。
		void Row(Wui::WuiContext& ctx, const Wui::WuiTheme& theme, float x, float y,
			const Wui::LocalizedLabel& label, const std::string& note = std::string())
		{
			Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme);
			if (!note.empty())
				Wui::Label(ctx, { x + 500.0f, y + 3.0f }, note, theme.TextMuted, 12.0f);
		}
	}

	// ---- 左侧分类列表(用户 2026-09-20:"还没有类型分类") ----
	void PreferencesPanel::DrawCategoryList(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		struct Entry { Category Category; const char* Key; const char* English; };
		const Entry entries[] = {
			{ Category::General,    "prefs.category.general",    "General" },
			{ Category::Appearance, "prefs.category.appearance", "Appearance" },
			{ Category::About,      "prefs.category.about",      "About" },
		};
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, theme.ContentBg, 0.0f });
		float y = rect.Y + 6.0f;
		for (const Entry& entry : entries)
		{
			const Wui::WuiRect row { rect.X + 4.0f, y, rect.W - 8.0f, 26.0f };
			const bool selected = m_Category == entry.Category;
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
			if (!localized.Term.empty())
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { row.X + 12.0f + ctx.MeasureTextWidth(label, 13.0f) + 6.0f,
					row.Y + 6.0f, 0, 0 }, theme.TextMuted, 0, 1.0f, localized.Term, 11.0f, false });
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
				m_Category = entry.Category;
			y += 28.0f;
		}
	}

	void PreferencesPanel::DrawGeneral(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		Editor::EditorPreferences& preferences = Editor::EditorPreferences::Get();
		const float x = rect.X;
		float y = rect.Y;

		// 界面语言(改完立即生效 + 自动落盘)。
		{
			std::vector<std::string> options;
			for (const LanguageOption& option : kLanguages)
				options.push_back(Wui::Tr(option.LabelKey, option.EnglishLabel));
			int selected = LanguageIndex(preferences.Data().Language);
			Row(ctx, theme, x, y, Wui::TrLabel("prefs.language", "Language"),
				Wui::Tr("prefs.language.note", "Applies immediately"));
			if (Wui::Combo(ctx, Wui::HashId("prefs.language"), { x + 300.0f, y, 180.0f, 24.0f },
				"Language", options, selected, theme))
			{
				preferences.SetLanguage(kLanguages[selected].Code);
				m_Status = Wui::Tr("prefs.autosave", "Saved automatically");
			}
			y += 32.0f;
		}

		// 英文术语对照(仅非英文界面有意义;英文界面说明原因,而不是给一个点了没用的开关)。
		{
			const Wui::LocalizedLabel label = Wui::TrLabel("prefs.term_hints", "Show English Terms");
			const bool languageIsEnglish = preferences.Data().Language.rfind("en", 0) == 0;
			if (languageIsEnglish)
			{
				Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.TextDisabled, 13.0f, theme);
			}
			else
			{
				bool enabled = preferences.Data().TermHints;
				const Wui::WuiRect row { x, y, 230.0f, 22.0f };
				if (Wui::Checkbox(ctx, Wui::HashId("prefs.term_hints"), row, label.Text, label.Term, enabled, theme))
				{
					preferences.SetShowTermHints(enabled);
					m_Status = Wui::Tr("prefs.autosave", "Saved automatically");
				}
			}
			Wui::Label(ctx, { x + 500.0f, y + 3.0f },
				Wui::Tr("prefs.term_hints.note", "Appears when the UI language is not English"),
				theme.TextMuted, 12.0f);
			y += 32.0f;
		}

		Wui::Label(ctx, { x, y + 2.0f },
			Wui::Tr("prefs.autosave", "All changes apply immediately and are saved automatically"),
			theme.TextMuted, 12.0f);
	}

	void PreferencesPanel::DrawAppearance(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		Editor::EditorPreferences& preferences = Editor::EditorPreferences::Get();
		const float x = rect.X;
		float y = rect.Y;

		{
			const std::vector<std::string> options {
				Wui::Tr("prefs.theme.dark", "Dark"),
				Wui::Tr("prefs.theme.light", "Light"),
				Wui::Tr("prefs.theme.system", "Follow System"),
			};
			int selected = static_cast<int>(preferences.Data().Theme);
			Row(ctx, theme, x, y, Wui::TrLabel("prefs.theme", "Theme"));
			if (Wui::Combo(ctx, Wui::HashId("prefs.theme"), { x + 300.0f, y, 180.0f, 24.0f },
				"Theme", options, selected, theme))
			{
				preferences.SetThemeMode(static_cast<Wui::WuiThemeMode>(selected));
				m_Status = Wui::Tr("prefs.autosave", "Saved automatically");
			}
			y += 32.0f;
		}

		{
			Row(ctx, theme, x, y, Wui::TrLabel("prefs.ui_scale", "UI Scale"),
				Wui::Tr("prefs.ui_scale.note", "Text only; layout is unchanged"));
			float value = preferences.Data().UiScale;
			if (Wui::DragFloat(ctx, Wui::HashId("prefs.ui_scale"), { x + 300.0f, y, 180.0f, 24.0f },
				value, 0.01f, 0.8f, 1.5f, theme))
				preferences.SetUiScale(value);
			y += 32.0f;
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
	}

	void PreferencesPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const float sidebarWidth = 170.0f;
		const Wui::WuiRect sidebar { rect.X, rect.Y, sidebarWidth, rect.H };
		const Wui::WuiRect content { rect.X + sidebarWidth + 12.0f, rect.Y + 10.0f,
			rect.W - sidebarWidth - 24.0f, rect.H - 20.0f };

		DrawCategoryList(ctx, sidebar, theme);

		switch (m_Category)
		{
			case Category::Appearance: DrawAppearance(ctx, content, theme); break;
			case Category::About: DrawAbout(ctx, content, theme); break;
			case Category::General:
			default: DrawGeneral(ctx, content, theme); break;
		}

		if (!m_Status.empty())
			Wui::Label(ctx, { content.X, content.Y + content.H - 18.0f }, m_Status, theme.Success, 12.0f);
	}
}
