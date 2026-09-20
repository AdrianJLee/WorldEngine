#include "wldpch.h"
#include "EditorPreferences.h"

#include "World/Core/Log.h"
#include "World/Settings/SettingsRegistry.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLocalization.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

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

		const char* RestoreModeToString(RestoreWindowsMode mode)
		{
			switch (mode)
			{
				case RestoreWindowsMode::Tabs: return "tabs";
				case RestoreWindowsMode::Layout: return "layout";
				case RestoreWindowsMode::None: return "none";
				case RestoreWindowsMode::Ask:
				default: return "ask";
			}
		}

		RestoreWindowsMode RestoreModeFromString(const std::string& value)
		{
			if (value == "tabs") return RestoreWindowsMode::Tabs;
			if (value == "layout") return RestoreWindowsMode::Layout;
			if (value == "none") return RestoreWindowsMode::None;
			return RestoreWindowsMode::Ask;
		}

		// 日志级别:偏好文件里存可读字符串("info"),面板用下拉;索引 = 下拉选项下标。
		struct LogLevelOption
		{
			const char* Value;
			const char* Label;
			spdlog::level::level_enum Level;
		};
		const LogLevelOption kLogLevels[] = {
			{ "trace", "Trace", spdlog::level::trace },
			{ "debug", "Debug", spdlog::level::debug },
			{ "info", "Info", spdlog::level::info },
			{ "warn", "Warn", spdlog::level::warn },
			{ "error", "Error", spdlog::level::err },
			{ "off", "Off", spdlog::level::off },
		};
		constexpr int kLogLevelCount = static_cast<int>(sizeof(kLogLevels) / sizeof(kLogLevels[0]));

		int ClampLogLevel(int level)
		{
			return std::clamp(level, 0, kLogLevelCount - 1);
		}

		int LogLevelFromString(const std::string& value)
		{
			for (int i = 0; i < kLogLevelCount; ++i)
				if (value == kLogLevels[i].Value)
					return i;
			return 2;   // info
		}

		// 诊断开关 → 环境变量:只在环境变量**未显式设置**时写入(环境变量 > 偏好文件)。
		void SetEnvIfUnset(const char* name, const char* value)
		{
			if (std::getenv(name))
				return;
			_putenv_s(name, value);
		}

		std::string FormatFloat(float value)
		{
			char buffer[32] = {};
			const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
			if (result.ec != std::errc())
				return std::to_string(value);
			return std::string(buffer, result.ptr);
		}

		std::string FormatBool(bool value) { return value ? "true" : "false"; }
	}

	EditorPreferences& EditorPreferences::Get()
	{
		static EditorPreferences preferences;
		return preferences;
	}

	void EditorPreferences::Load(const std::filesystem::path& path)
	{
		m_Path = path;
		std::ifstream file(path, std::ios::binary);
		std::optional<Wui::JsonValue> root;
		if (!file)
			WLD_CORE_INFO("编辑器偏好:未找到 {0},使用默认值(英文/暗色/随 DPI 缩放)", path.string());
		else
		{
			std::ostringstream buffer;
			buffer << file.rdbuf();
			std::string error;
			root = Wui::JsonValue::Parse(buffer.str(), &error);
			if (!root || root->type != Wui::JsonValue::Type::Object)
				WLD_CORE_WARN("编辑器偏好解析失败({0}): {1}(使用默认值)", path.string(), error);
			else
				for (const auto& [key, value] : root->Object)
				{
					const bool isBool = value.type == Wui::JsonValue::Type::Bool;
					const bool isNumber = value.type == Wui::JsonValue::Type::Number;
					const bool isString = value.type == Wui::JsonValue::Type::String;
					if (key == "language" && isString)
						m_Data.Language = value.String;
					else if (key == "theme" && isString)
						m_Data.Theme = ThemeFromString(value.String);
					else if (key == "ui_scale" && isNumber)
						m_Data.UiScale = std::clamp(static_cast<float>(value.Number), 0.8f, 1.8f);
					else if (key == "term_hints" && isBool)
						m_Data.TermHints = value.Bool;
					else if (key == "script_font_size" && isNumber)
						m_Data.ScriptFontSize = std::clamp(static_cast<float>(value.Number), 10.0f, 32.0f);
					else if (key == "asset_hot_reload" && isBool)
						m_Data.AssetHotReload = value.Bool;
					else if (key == "restore_windows" && isString)
						m_Data.RestoreWindows = RestoreModeFromString(value.String);
					else if (key == "ai_control_port" && isNumber)
						m_Data.AiControlPort = std::clamp(static_cast<int>(value.Number), 0, 65535);
					else if (key == "log_level" && isString)
						m_Data.LogLevel = LogLevelFromString(value.String);
					else if (key == "diag_frame_timing" && isBool)
						m_Data.DiagFrameTiming = value.Bool;
					else if (key == "diag_present_trace" && isBool)
						m_Data.DiagPresentTrace = value.Bool;
					else if (key == "diag_gl_trace" && isBool)
						m_Data.DiagGlTrace = value.Bool;
					else if (key == "diag_asset_trace" && isBool)
						m_Data.DiagAssetTrace = value.Bool;
					else if (key == "diag_vulkan_validation" && isBool)
						m_Data.DiagVulkanValidation = value.Bool;
				}
		}
		// 开发/自动化覆盖:环境变量优先于**偏好文件**(与渲染设置的"环境变量 > 清单"同口径);
		// 必须在解析之后再套用,否则会被文件/默认值覆盖(实测踩过:WLD_LANG 与 WLD_UI_SCALE 都踩过)。
		if (const char* language = std::getenv("WLD_LANG"))
			if (language[0])
				m_Data.Language = language;
		if (const char* theme = std::getenv("WLD_UI_THEME"))
		{
			if (std::strcmp(theme, "light") == 0) m_Data.Theme = Wui::WuiThemeMode::Light;
			else if (std::strcmp(theme, "system") == 0) m_Data.Theme = Wui::WuiThemeMode::System;
			else if (std::strcmp(theme, "dark") == 0) m_Data.Theme = Wui::WuiThemeMode::Dark;
		}
		if (const char* scale = std::getenv("WLD_UI_SCALE"))
		{
			const float parsed = static_cast<float>(std::atof(scale));
			if (parsed >= 0.5f && parsed <= 2.0f)
				m_Data.UiScale = parsed;
		}
		if (const char* hints = std::getenv("WLD_UI_TERM_HINTS"))
			m_Data.TermHints = !(hints[0] == '0' || hints[0] == '\0');
		// 启动恢复策略:自动化/脚本可钉死(ask 会让无人值守的启动弹出一个模态)。
		if (const char* restore = std::getenv("WLD_RESTORE_WINDOWS"))
			if (restore[0])
				m_Data.RestoreWindows = RestoreModeFromString(restore);
		WLD_CORE_INFO("编辑器偏好已加载: {0}(language={1} theme={2} scale={3:.2f} termHints={4} scriptFont={5:.0f} hotReload={6} aiPort={7} log={8})",
			path.string(), m_Data.Language, ThemeToString(m_Data.Theme), m_Data.UiScale,
			m_Data.TermHints ? 1 : 0, m_Data.ScriptFontSize, m_Data.AssetHotReload ? 1 : 0,
			m_Data.AiControlPort, kLogLevels[ClampLogLevel(m_Data.LogLevel)].Value);
		Apply();
		// 诊断开关必须在窗口/渲染后端创建**之前**写进环境变量(它们在启动期读取)。
		ApplyDiagnosticEnvironment();
		// 偏好项全部注册进设置注册表:面板按注册表渲染,不再有"只能改文件"的编辑器开关。
		RegisterSettings();
	}

	void EditorPreferences::ApplyDiagnosticEnvironment() const
	{
		SetEnvIfUnset("WLD_FRAME_TIMING", m_Data.DiagFrameTiming ? "1" : "0");
		SetEnvIfUnset("WLD_ASSET_HOTRELOAD_TRACE", m_Data.DiagAssetTrace ? "1" : "0");
		SetEnvIfUnset("WLD_VULKAN_VALIDATION", m_Data.DiagVulkanValidation ? "1" : "0");
		// 这两条是"存在即开"的语义:只在开启时写,关闭时不动环境(默认就是关)。
		if (m_Data.DiagPresentTrace)
			SetEnvIfUnset("WLD_VK_PRESENT_TRACE", "1");
		if (m_Data.DiagGlTrace)
			SetEnvIfUnset("WLD_GL_TRACE_DRAW", "1");
	}

	void EditorPreferences::Apply() const
	{
		Wui::SetLanguage(m_Data.Language);
		Wui::SetThemeMode(m_Data.Theme);
		Wui::SetUiScale(m_Data.UiScale);
		Wui::SetShowTermHints(m_Data.TermHints);
		// 日志级别是唯一"改了立刻能观察到"的诊断项:直接改 spdlog 级别。
		const spdlog::level::level_enum level = kLogLevels[ClampLogLevel(m_Data.LogLevel)].Level;
		if (auto logger = Log::GetCoreLogger())
			logger->set_level(level);
		if (auto logger = Log::GetClientLogger())
			logger->set_level(level);
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
			<< "  \"ui_scale\": " << FormatFloat(m_Data.UiScale) << ",\n"
			<< "  \"term_hints\": " << FormatBool(m_Data.TermHints) << ",\n"
			<< "  \"script_font_size\": " << FormatFloat(m_Data.ScriptFontSize) << ",\n"
			<< "  \"asset_hot_reload\": " << FormatBool(m_Data.AssetHotReload) << ",\n"
			<< "  \"restore_windows\": \"" << RestoreModeToString(m_Data.RestoreWindows) << "\",\n"
			<< "  \"ai_control_port\": " << m_Data.AiControlPort << ",\n"
			<< "  \"log_level\": \"" << kLogLevels[ClampLogLevel(m_Data.LogLevel)].Value << "\",\n"
			<< "  \"diag_frame_timing\": " << FormatBool(m_Data.DiagFrameTiming) << ",\n"
			<< "  \"diag_present_trace\": " << FormatBool(m_Data.DiagPresentTrace) << ",\n"
			<< "  \"diag_gl_trace\": " << FormatBool(m_Data.DiagGlTrace) << ",\n"
			<< "  \"diag_asset_trace\": " << FormatBool(m_Data.DiagAssetTrace) << ",\n"
			<< "  \"diag_vulkan_validation\": " << FormatBool(m_Data.DiagVulkanValidation) << "\n"
			<< "}\n";
		return static_cast<bool>(file);
	}

	void EditorPreferences::Commit()
	{
		++m_Generation;
		std::string error;
		if (!Save(&error))
			WLD_CORE_WARN("编辑器偏好保存失败: {0}", error);
	}

	void EditorPreferences::SetLanguage(const std::string& language)
	{
		if (m_Data.Language == language)
			return;
		m_Data.Language = language;
		Apply();
		Commit();
	}

	void EditorPreferences::SetThemeMode(Wui::WuiThemeMode mode)
	{
		if (m_Data.Theme == mode)
			return;
		m_Data.Theme = mode;
		Apply();
		Commit();
	}

	void EditorPreferences::SetUiScale(float scale)
	{
		const float clamped = std::clamp(scale, 0.8f, 1.8f);
		if (std::abs(m_Data.UiScale - clamped) < 0.001f)
			return;
		m_Data.UiScale = clamped;
		Apply();
		Commit();
	}

	void EditorPreferences::SetShowTermHints(bool enabled)
	{
		if (m_Data.TermHints == enabled)
			return;
		m_Data.TermHints = enabled;
		Apply();
		Commit();
	}

	void EditorPreferences::SetScriptFontSize(float size)
	{
		const float clamped = std::clamp(size, 10.0f, 32.0f);
		if (std::abs(m_Data.ScriptFontSize - clamped) < 0.01f)
			return;
		m_Data.ScriptFontSize = clamped;
		Commit();
	}

	void EditorPreferences::SetAssetHotReload(bool enabled)
	{
		if (m_Data.AssetHotReload == enabled)
			return;
		m_Data.AssetHotReload = enabled;
		Commit();
	}

	void EditorPreferences::SetRestoreWindows(RestoreWindowsMode mode)
	{
		if (m_Data.RestoreWindows == mode)
			return;
		m_Data.RestoreWindows = mode;
		Commit();
	}

	void EditorPreferences::SetAiControlPort(int port)
	{
		const int clamped = std::clamp(port, 0, 65535);
		if (m_Data.AiControlPort == clamped)
			return;
		m_Data.AiControlPort = clamped;
		Commit();
	}

	void EditorPreferences::SetLogLevel(int level)
	{
		const int clamped = ClampLogLevel(level);
		if (m_Data.LogLevel == clamped)
			return;
		m_Data.LogLevel = clamped;
		Apply();
		Commit();
	}

	void EditorPreferences::SetDiagFrameTiming(bool enabled)
	{
		if (m_Data.DiagFrameTiming == enabled) return;
		m_Data.DiagFrameTiming = enabled;
		Commit();
	}

	void EditorPreferences::SetDiagPresentTrace(bool enabled)
	{
		if (m_Data.DiagPresentTrace == enabled) return;
		m_Data.DiagPresentTrace = enabled;
		Commit();
	}

	void EditorPreferences::SetDiagGlTrace(bool enabled)
	{
		if (m_Data.DiagGlTrace == enabled) return;
		m_Data.DiagGlTrace = enabled;
		Commit();
	}

	void EditorPreferences::SetDiagAssetTrace(bool enabled)
	{
		if (m_Data.DiagAssetTrace == enabled) return;
		m_Data.DiagAssetTrace = enabled;
		Commit();
	}

	void EditorPreferences::SetDiagVulkanValidation(bool enabled)
	{
		if (m_Data.DiagVulkanValidation == enabled) return;
		m_Data.DiagVulkanValidation = enabled;
		Commit();
	}

	// ---- 设置注册表:偏好项(Editor 作用域) ----
	//
	// 面板按 Group 分组渲染这些行:Id/类型/选项/生效时机/tooltip 全部来自这里,
	// 不再各写一份控件代码。新增偏好项 = 加一条 + 加两个中文键(settings.<Id>[.tooltip])。
	// 注:引擎是 C++17,没有指定初始值设定项(designated initializers),用下面的
	// addBool/addNumber/addEnum 收口字段顺序与样板。
	void EditorPreferences::RegisterSettings()
	{
		static bool registered = false;
		if (registered)
			return;
		registered = true;

		using Settings::SettingApply;
		using Settings::SettingDescriptor;
		using Settings::SettingOption;
		using Settings::SettingsRegistry;
		using Settings::SettingScope;
		using Settings::SettingType;
		using WriteFn = std::function<bool(const std::string&, std::string*)>;
		using ReadFn = std::function<std::string()>;
		using FlagFn = std::function<bool()>;

		SettingsRegistry& registry = SettingsRegistry::Get();
		EditorPreferences& prefs = EditorPreferences::Get();

		auto describe = [](const char* id, const char* group, SettingType type, SettingApply apply,
			const char* label, const char* tooltip)
		{
			SettingDescriptor descriptor;
			descriptor.Id = id;
			descriptor.Group = group;
			descriptor.Type = type;
			descriptor.Scope = SettingScope::Editor;
			descriptor.Apply = apply;
			descriptor.Label = label;
			descriptor.Tooltip = tooltip;
			return descriptor;
		};

		auto addBool = [&registry, &describe](const char* id, const char* group, SettingApply apply,
			const char* label, const char* tooltip, ReadFn read, WriteFn write, FlagFn isDefault, FlagFn reset,
			bool advanced = false, FlagFn isEnabled = FlagFn {}, std::string disabledReason = std::string())
		{
			SettingDescriptor descriptor = describe(id, group, SettingType::Bool, apply, label, tooltip);
			descriptor.Read = std::move(read);
			descriptor.Write = std::move(write);
			descriptor.IsDefault = std::move(isDefault);
			descriptor.Reset = std::move(reset);
			descriptor.Advanced = advanced;
			descriptor.IsEnabled = std::move(isEnabled);
			descriptor.DisabledReason = std::move(disabledReason);
			registry.Register(std::move(descriptor));
		};

		auto addNumber = [&registry, &describe](const char* id, const char* group, SettingType type, SettingApply apply,
			const char* label, const char* tooltip, const char* unit, double min, double max, double step,
			ReadFn read, WriteFn write, FlagFn isDefault, FlagFn reset, bool advanced = false)
		{
			SettingDescriptor descriptor = describe(id, group, type, apply, label, tooltip);
			descriptor.Unit = unit;
			descriptor.Min = min;
			descriptor.Max = max;
			descriptor.Step = step;
			descriptor.Read = std::move(read);
			descriptor.Write = std::move(write);
			descriptor.IsDefault = std::move(isDefault);
			descriptor.Reset = std::move(reset);
			descriptor.Advanced = advanced;
			registry.Register(std::move(descriptor));
		};

		auto addEnum = [&registry, &describe](const char* id, const char* group, SettingApply apply,
			const char* label, const char* tooltip, std::vector<SettingOption> options,
			ReadFn read, WriteFn write, FlagFn isDefault, FlagFn reset, bool advanced = false)
		{
			SettingDescriptor descriptor = describe(id, group, SettingType::Enum, apply, label, tooltip);
			descriptor.Options = std::move(options);
			descriptor.Read = std::move(read);
			descriptor.Write = std::move(write);
			descriptor.IsDefault = std::move(isDefault);
			descriptor.Reset = std::move(reset);
			descriptor.Advanced = advanced;
			registry.Register(std::move(descriptor));
		};

		// ---- 通用 General ----
		addEnum("editor.general.language", "General", SettingApply::Immediate, "Language",
			"Language\nSwitches the language used by the editor UI and panel text.\nDefault: English. Applies immediately.",
			{ SettingOption { "en", "English" }, SettingOption { "zh-CN", "简体中文" } },
			[&prefs] { return prefs.Data().Language; },
			[&prefs](const std::string& value, std::string*) { prefs.SetLanguage(value); return true; },
			[&prefs] { return prefs.Data().Language == "en"; },
			[&prefs] { prefs.SetLanguage("en"); return true; });

		addBool("editor.general.term_hints", "General", SettingApply::Immediate, "Show English Terms",
			"Show English terms\nAppends the English term after translated feature/component/parameter names so docs and scripts can be cross-checked.\nDefault: on (only applies when the UI language is not English). Applies immediately.",
			[&prefs] { return FormatBool(prefs.Data().TermHints); },
			[&prefs](const std::string& value, std::string*) { prefs.SetShowTermHints(value == "true"); return true; },
			[&prefs] { return prefs.Data().TermHints; },
			[&prefs] { prefs.SetShowTermHints(true); return true; },
			false,
			[&prefs] { return prefs.Data().Language.rfind("en", 0) != 0; },
			"Only applies when the interface language is not English.");

		// ---- 外观 Appearance ----
		addEnum("editor.appearance.theme", "Appearance", SettingApply::Immediate, "Theme",
			"Theme\nDark is the default; Light and Follow System are also available.\nDefault: Dark. Applies immediately.",
			{ SettingOption { "dark", "Dark" }, SettingOption { "light", "Light" },
				SettingOption { "system", "Follow System" } },
			[&prefs] { return std::string(ThemeToString(prefs.Data().Theme)); },
			[&prefs](const std::string& value, std::string*) { prefs.SetThemeMode(ThemeFromString(value)); return true; },
			[&prefs] { return prefs.Data().Theme == Wui::WuiThemeMode::Dark; },
			[&prefs] { prefs.SetThemeMode(Wui::WuiThemeMode::Dark); return true; });

		addNumber("editor.appearance.ui_scale", "Appearance", SettingType::Float, SettingApply::Immediate,
			"UI Scale",
			"UI scale\nScales the whole UI (layout + controls + text) together; the default is derived from the display DPI.\nRange: 0.8 - 1.8. Applies immediately.",
			"x", 0.8, 1.8, 0.01,
			[&prefs] { return FormatFloat(prefs.Data().UiScale); },
			[&prefs](const std::string& value, std::string*) { prefs.SetUiScale(static_cast<float>(std::atof(value.c_str()))); return true; },
			[&prefs] { return std::abs(prefs.Data().UiScale - 1.30f) < 0.01f; },
			[&prefs] { prefs.SetUiScale(1.30f); return true; });

		// ---- 编辑器 Editor ----
		addNumber("editor.editor.script_font_size", "Editor", SettingType::Float, SettingApply::Immediate,
			"Script Editor Font Size",
			"Script editor font size\nFont size used by script windows (Ctrl+wheel still zooms temporarily).\nRange: 10 - 32 px. Applies immediately.",
			"px", 10.0, 32.0, 1.0,
			[&prefs] { return FormatFloat(prefs.Data().ScriptFontSize); },
			[&prefs](const std::string& value, std::string*) { prefs.SetScriptFontSize(static_cast<float>(std::atof(value.c_str()))); return true; },
			[&prefs] { return std::abs(prefs.Data().ScriptFontSize - 14.0f) < 0.01f; },
			[&prefs] { prefs.SetScriptFontSize(14.0f); return true; });

		// ---- 工作流 Workflow ----
		addBool("editor.workflow.asset_hot_reload", "Workflow", SettingApply::Immediate, "Asset Hot Reload",
			"Asset hot reload\nRe-imports textures/materials/models after they change on disk (content-hash based; rewriting identical content does not trigger).\nDefault: on (the WLD_ASSET_HOTRELOAD environment variable overrides). Applies immediately.",
			[&prefs] { return FormatBool(prefs.Data().AssetHotReload); },
			[&prefs](const std::string& value, std::string*) { prefs.SetAssetHotReload(value == "true"); return true; },
			[&prefs] { return prefs.Data().AssetHotReload; },
			[&prefs] { prefs.SetAssetHotReload(true); return true; });

		// ---- 自动化 Automation ----
		// 启动恢复上次的独立窗口:默认 Ask(用户 2026-09-20:"默认设置应该是询问")。
		addEnum("editor.workflow.restore_windows", "Workflow", SettingApply::Restart, "Restore Open Windows",
			"启动时如何处理上次开着的独立窗口\n默认:询问(启动时问一次,可勾\"记住我的选择\")。\n"
			"恢复为顶栏标签 = 不弹窗口、不抢焦点;按上次形态 = 浮窗仍在但不会抢焦点。\n重启编辑器后生效",
			{ SettingOption { "ask", "Ask Every Time" },
				SettingOption { "tabs", "Restore as Top-Bar Tabs" },
				SettingOption { "layout", "Restore Previous Layout" },
				SettingOption { "none", "Don't Restore" } },
			[&prefs] { return std::string(RestoreModeToString(prefs.Data().RestoreWindows)); },
			[&prefs](const std::string& value, std::string*) { prefs.SetRestoreWindows(RestoreModeFromString(value)); return true; },
			[&prefs] { return prefs.Data().RestoreWindows == RestoreWindowsMode::Ask; },
			[&prefs] { prefs.SetRestoreWindows(RestoreWindowsMode::Ask); return true; });

		addNumber("editor.automation.ai_control_port", "Automation", SettingType::Int, SettingApply::Restart,
			"AI Control Port",
			"AI control port\nListens on 127.0.0.1 only; lets scripts/AI read the accessibility tree and take screenshots; 0 = off. The --ai-control=<port> argument takes precedence.\nDefault: 0 (off). Takes effect after restarting the editor.",
			"", 0.0, 65535.0, 1.0,
			[&prefs] { return std::to_string(prefs.Data().AiControlPort); },
			[&prefs](const std::string& value, std::string*) { prefs.SetAiControlPort(std::atoi(value.c_str())); return true; },
			[&prefs] { return prefs.Data().AiControlPort == 0; },
			[&prefs] { prefs.SetAiControlPort(0); return true; });

		// ---- 诊断 Diagnostics ----
		addEnum("editor.diagnostics.log_level", "Diagnostics", SettingApply::Immediate, "Log Level",
			"Log level\nMinimum level for the console and the log file; use Trace/Debug when investigating.\nDefault: Info. Applies immediately.",
			{ SettingOption { "trace", "Trace" }, SettingOption { "debug", "Debug" },
				SettingOption { "info", "Info" }, SettingOption { "warn", "Warn" },
				SettingOption { "error", "Error" }, SettingOption { "off", "Off" } },
			[&prefs] { return std::string(kLogLevels[ClampLogLevel(prefs.Data().LogLevel)].Value); },
			[&prefs](const std::string& value, std::string*) { prefs.SetLogLevel(LogLevelFromString(value)); return true; },
			[&prefs] { return prefs.Data().LogLevel == 2; },
			[&prefs] { prefs.SetLogLevel(2); return true; });

		addBool("editor.diagnostics.frame_timing", "Diagnostics", SettingApply::Restart, "Frame Timing",
			"Frame timing\nSplits a frame into scene update / UI / record-submit phases and prints their timings (WLD_FRAME_TIMING). Debug only.\nDefault: off. Takes effect after restarting the editor.",
			[&prefs] { return FormatBool(prefs.Data().DiagFrameTiming); },
			[&prefs](const std::string& value, std::string*) { prefs.SetDiagFrameTiming(value == "true"); return true; },
			[&prefs] { return !prefs.Data().DiagFrameTiming; },
			[&prefs] { prefs.SetDiagFrameTiming(false); return true; }, true);

		addBool("editor.diagnostics.present_trace", "Diagnostics", SettingApply::Restart, "Present Trace",
			"Present trace\nPrints acquire/present state every frame (WLD_VK_PRESENT_TRACE) to diagnose blank windows or present failures. Debug only.\nDefault: off. Takes effect after restarting the editor.",
			[&prefs] { return FormatBool(prefs.Data().DiagPresentTrace); },
			[&prefs](const std::string& value, std::string*) { prefs.SetDiagPresentTrace(value == "true"); return true; },
			[&prefs] { return !prefs.Data().DiagPresentTrace; },
			[&prefs] { prefs.SetDiagPresentTrace(false); return true; }, true);

		addBool("editor.diagnostics.gl_trace", "Diagnostics", SettingApply::Restart, "OpenGL Draw Trace",
			"OpenGL draw trace\nLogs texture/FBO/program bindings and draw calls (WLD_GL_TRACE_DRAW) to diagnose black screens and wrong frames. Debug only.\nDefault: off. Takes effect after restarting the editor.",
			[&prefs] { return FormatBool(prefs.Data().DiagGlTrace); },
			[&prefs](const std::string& value, std::string*) { prefs.SetDiagGlTrace(value == "true"); return true; },
			[&prefs] { return !prefs.Data().DiagGlTrace; },
			[&prefs] { prefs.SetDiagGlTrace(false); return true; }, true);

		addBool("editor.diagnostics.asset_trace", "Diagnostics", SettingApply::Restart, "Asset Hot Reload Trace",
			"Asset hot reload trace\nPrints every hot-reload decision (content hash / re-import) in detail (WLD_ASSET_HOTRELOAD_TRACE). Debug only.\nDefault: off. Takes effect after restarting the editor.",
			[&prefs] { return FormatBool(prefs.Data().DiagAssetTrace); },
			[&prefs](const std::string& value, std::string*) { prefs.SetDiagAssetTrace(value == "true"); return true; },
			[&prefs] { return !prefs.Data().DiagAssetTrace; },
			[&prefs] { prefs.SetDiagAssetTrace(false); return true; }, true);

		addBool("editor.diagnostics.vulkan_validation", "Diagnostics", SettingApply::Restart, "Vulkan Validation",
			"Vulkan validation\nPrints VUID violations and object names (WLD_VULKAN_VALIDATION); device-lost forensics relies on it, so keep it on except while measuring performance.\nDefault: on. Takes effect after restarting the editor.",
			[&prefs] { return FormatBool(prefs.Data().DiagVulkanValidation); },
			[&prefs](const std::string& value, std::string*) { prefs.SetDiagVulkanValidation(value == "true"); return true; },
			[&prefs] { return prefs.Data().DiagVulkanValidation; },
			[&prefs] { prefs.SetDiagVulkanValidation(true); return true; }, true);

		WLD_CORE_INFO("设置注册表:已注册编辑器偏好 {0} 项", registry.OfScope(SettingScope::Editor).size());
	}
}
