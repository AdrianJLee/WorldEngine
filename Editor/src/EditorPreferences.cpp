#include "wldpch.h"
#include "EditorPreferences.h"

#include "World/WUI/WuiLocalization.h"
#include "World/WUI/WuiJson.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace World::Editor
{
	namespace
	{
		const char* ThemeToString(Wui::WuiThemeMode mode)
		{
			switch (mode)
			{
				case Wui::WuiThemeMode::Light: return "light";
				case Wui::WuiThemeMode::System: return "system";
				case Wui::WuiThemeMode::Dark:
				default: return "dark";
			}
		}

		Wui::WuiThemeMode ThemeFromString(const std::string& value)
		{
			if (value == "light") return Wui::WuiThemeMode::Light;
			if (value == "system") return Wui::WuiThemeMode::System;
			return Wui::WuiThemeMode::Dark;
		}
	}

	EditorPreferences& EditorPreferences::Get()
	{
		static EditorPreferences preferences;
		return preferences;
	}

	void EditorPreferences::Load(const std::filesystem::path& path)
	{
		m_Path = path;
		// 开发/自动化覆盖:`WLD_LANG` 优先于偏好文件(与渲染设置的"环境变量 > 清单"同口径)。
		// 必须在读文件**之前**生效:偏好文件不存在时也要能覆盖(自动化常用全新工作区)。
		if (const char* language = std::getenv("WLD_LANG"))
			if (language[0])
				m_Data.Language = language;
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			WLD_CORE_INFO("编辑器偏好:未找到 {0},使用默认值(英文/暗色/1.15)", path.string());
			Apply();
			return;
		}
		std::ostringstream buffer;
		buffer << file.rdbuf();
		std::string error;
		const std::optional<Wui::JsonValue> root = Wui::JsonValue::Parse(buffer.str(), &error);
		if (!root || root->type != Wui::JsonValue::Type::Object)
		{
			WLD_CORE_WARN("编辑器偏好解析失败({0}): {1}(使用默认值)", path.string(), error);
			Apply();
			return;
		}
		for (const auto& [key, value] : root->Object)
		{
			if (key == "language" && value.type == Wui::JsonValue::Type::String)
				m_Data.Language = value.String;
			else if (key == "theme" && value.type == Wui::JsonValue::Type::String)
				m_Data.Theme = ThemeFromString(value.String);
			else if (key == "ui_scale" && value.type == Wui::JsonValue::Type::Number)
				m_Data.UiScale = std::clamp(static_cast<float>(value.Number), 0.8f, 1.5f);
			else if (key == "term_hints" && value.type == Wui::JsonValue::Type::Bool)
				m_Data.TermHints = value.Bool;
		}
		WLD_CORE_INFO("编辑器偏好已加载: {0}(language={1} theme={2} scale={3:.2f} termHints={4})",
			path.string(), m_Data.Language, ThemeToString(m_Data.Theme), m_Data.UiScale,
			m_Data.TermHints ? 1 : 0);
		Apply();
	}

	void EditorPreferences::Apply() const
	{
		Wui::SetLanguage(m_Data.Language);
		Wui::SetThemeMode(m_Data.Theme);
		Wui::SetUiFontScale(m_Data.UiScale);
		Wui::SetShowTermHints(m_Data.TermHints);
	}

	bool EditorPreferences::Save(std::string* error) const
	{
		if (m_Path.empty())
		{
			if (error) *error = "editor preferences path is empty";
			return false;
		}
		std::ofstream file(m_Path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			if (error) *error = "cannot write " + m_Path.string();
			return false;
		}
		file << "{\n"
			<< "  \"language\": \"" << m_Data.Language << "\",\n"
			<< "  \"theme\": \"" << ThemeToString(m_Data.Theme) << "\",\n"
			<< "  \"ui_scale\": " << m_Data.UiScale << ",\n"
			<< "  \"term_hints\": " << (m_Data.TermHints ? "true" : "false") << "\n"
			<< "}\n";
		return static_cast<bool>(file);
	}

	void EditorPreferences::SetLanguage(const std::string& language)
	{
		if (m_Data.Language == language)
			return;
		m_Data.Language = language;
		Apply();
		std::string error;
		if (!Save(&error))
			WLD_CORE_WARN("编辑器偏好保存失败: {0}", error);
	}

	void EditorPreferences::SetThemeMode(Wui::WuiThemeMode mode)
	{
		if (m_Data.Theme == mode)
			return;
		m_Data.Theme = mode;
		Apply();
		std::string error;
		if (!Save(&error))
			WLD_CORE_WARN("编辑器偏好保存失败: {0}", error);
	}

	void EditorPreferences::SetUiScale(float scale)
	{
		const float clamped = std::clamp(scale, 0.8f, 1.5f);
		if (std::abs(m_Data.UiScale - clamped) < 0.001f)
			return;
		m_Data.UiScale = clamped;
		Apply();
		std::string error;
		if (!Save(&error))
			WLD_CORE_WARN("编辑器偏好保存失败: {0}", error);
	}

	void EditorPreferences::SetShowTermHints(bool enabled)
	{
		if (m_Data.TermHints == enabled)
			return;
		m_Data.TermHints = enabled;
		Apply();
		std::string error;
		if (!Save(&error))
			WLD_CORE_WARN("编辑器偏好保存失败: {0}", error);
	}
}
