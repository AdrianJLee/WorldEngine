#pragma once

#include "World/WUI/WuiWidgets.h"

#include <filesystem>
#include <string>

namespace World::Editor
{
	// 编辑器用户偏好(P4-UX1):**用户级**,不随项目提交 —— 存 `Editor/editor-prefs.json`。
	// 与 `Game/project.we.yaml`(项目级)严格分家:语言/主题/密度/字号属于"这台机器上的这个人"。
	//
	// 交互口径:任何一次修改**立即应用 + 立即落盘**(自动保存),不需要"保存"按钮 ——
	// 用户改了就是改了,面板只显示"已自动保存"状态。
	struct EditorPreferencesData
	{
		std::string Language = "en";                    // en / zh-CN(目录见 Editor/assets/localization)
		Wui::WuiThemeMode Theme = Wui::WuiThemeMode::Dark;
		float UiScale = 1.15f;                          // 0.8..1.5
		bool TermHints = true;                          // 非英文界面下显示英文术语对照
	};

	class EditorPreferences
	{
	public:
		static EditorPreferences& Get();

		void Load(const std::filesystem::path& path);
		const EditorPreferencesData& Data() const { return m_Data; }
		const std::filesystem::path& Path() const { return m_Path; }

		// 修改 = 应用 + 落盘(失败只告警,不影响本次会话)。
		void SetLanguage(const std::string& language);
		void SetThemeMode(Wui::WuiThemeMode mode);
		void SetUiScale(float scale);
		void SetShowTermHints(bool enabled);

		// 把当前值应用到 WUI(主题/字号/语言/术语对照)。
		void Apply() const;
		bool Save(std::string* error = nullptr) const;

	private:
		std::filesystem::path m_Path;
		EditorPreferencesData m_Data;
	};
}

