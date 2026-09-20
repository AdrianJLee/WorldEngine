#include "wldpch.h"
#include "PreferencesPanel.h"

#include "../../EditorPreferences.h"
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
	}

	void PreferencesPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		Editor::EditorPreferences& preferences = Editor::EditorPreferences::Get();
		const Wui::WuiTheme& theme = host.Theme();
		const float x = rect.X + 10.0f;
		const float width = rect.W - 20.0f;
		float y = rect.Y + 10.0f;

		{
			const Wui::LocalizedLabel header = Wui::TrLabel("prefs.group.general", "General");
			Wui::LabelWithTerm(ctx, { x, y }, header.Text, header.Term, theme.TextMuted, 12.0f, theme);
			y += 22.0f;
		}

		// 界面语言(改完立即生效 + 落盘:不需要保存按钮)。
		{
			std::vector<std::string> options;
			for (const LanguageOption& option : kLanguages)
				options.push_back(Wui::Tr(option.LabelKey, option.EnglishLabel));
			int selected = LanguageIndex(preferences.Data().Language);
			const Wui::LocalizedLabel label = Wui::TrLabel("prefs.language", "Language");
			Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme);
			if (Wui::Combo(ctx, Wui::HashId("prefs.language"), { x + 170.0f, y, 150.0f, 22.0f },
				label.Text, options, selected, theme))
			{
				preferences.SetLanguage(kLanguages[selected].Code);
				m_Status = Wui::Tr("prefs.autosave", "All changes apply immediately and are saved automatically");
			}
			y += 30.0f;
		}

		// 英文术语对照(中文界面才有意义;英文界面下禁用并说明原因)。
		{
			const Wui::LocalizedLabel label = Wui::TrLabel("prefs.term_hints", "Show English Terms");
			const bool languageIsEnglish = preferences.Data().Language.rfind("en", 0) == 0;
			if (languageIsEnglish)
			{
				// 英文界面下术语对照没有意义:不给一个点了也不生效的复选框,直接说明。
				Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.TextDisabled, 13.0f, theme);
				Wui::Label(ctx, { x + 220.0f, y + 3.0f },
					Wui::Tr("prefs.term_hints.note", "Chinese UI shows the English term next to each label"),
					theme.TextMuted, 12.0f);
			}
			else
			{
				bool enabled = preferences.Data().TermHints;
				const Wui::WuiRect row { x, y, width * 0.6f, 20.0f };
				if (Wui::Checkbox(ctx, Wui::HashId("prefs.term_hints"), row, label.Text, label.Term, enabled, theme))
				{
					preferences.SetShowTermHints(enabled);
					m_Status = Wui::Tr("prefs.autosave", "All changes apply immediately and are saved automatically");
				}
				Wui::Label(ctx, { x + 220.0f, y + 3.0f },
					Wui::Tr("prefs.term_hints.note", "Chinese UI shows the English term next to each label"),
					theme.TextMuted, 12.0f);
			}
			y += 30.0f;
		}

		// 主题(暗/浅/跟随系统)。
		{
			const std::vector<std::string> options {
				Wui::Tr("prefs.theme.dark", "Dark"),
				Wui::Tr("prefs.theme.light", "Light"),
				Wui::Tr("prefs.theme.system", "Follow System"),
			};
			int selected = static_cast<int>(preferences.Data().Theme);
			const Wui::LocalizedLabel label = Wui::TrLabel("prefs.theme", "Theme");
			Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme);
			if (Wui::Combo(ctx, Wui::HashId("prefs.theme"), { x + 170.0f, y, 150.0f, 22.0f },
				label.Text, options, selected, theme))
			{
				preferences.SetThemeMode(static_cast<Wui::WuiThemeMode>(selected));
				m_Status = Wui::Tr("prefs.autosave", "All changes apply immediately and are saved automatically");
			}
			y += 30.0f;
		}

		// 界面缩放(0.8..1.5,只放大文字,不改布局数值)。
		{
			const Wui::LocalizedLabel label = Wui::TrLabel("prefs.ui_scale", "UI Scale");
			Wui::LabelWithTerm(ctx, { x, y + 3.0f }, label.Text, label.Term, theme.Text, 13.0f, theme);
			float value = preferences.Data().UiScale;
			if (Wui::DragFloat(ctx, Wui::HashId("prefs.ui_scale"), { x + 170.0f, y, 150.0f, 22.0f },
				value, 0.01f, 0.8f, 1.5f, theme))
			{
				preferences.SetUiScale(value);
			}
			y += 30.0f;
		}

		y += 4.0f;
		Wui::Label(ctx, { x, y }, Wui::Tr("prefs.autosave", "All changes apply immediately and are saved automatically"),
			theme.TextMuted, 12.0f);
		y += 18.0f;
		if (!m_Status.empty())
		{
			Wui::Label(ctx, { x, y }, m_Status, theme.Success, 12.0f);
			y += 18.0f;
		}
		Wui::Label(ctx, { x, y }, std::string(Wui::Tr("prefs.path", "Preferences file")) + ": "
			+ preferences.Path().string(), theme.TextMuted, 11.0f);
	}
}
