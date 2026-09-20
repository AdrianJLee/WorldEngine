#include "wldpch.h"
#include "World/WUI/WuiWidgets.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace World::Wui
{
	// 提前声明:State() 的惰性初始化要用它(定义在本文件末尾,公开 API)。
	WuiTheme ResolveTheme(WuiThemeMode mode);

	namespace
	{
		WuiTheme MakeDarkTheme()
		{
			// 令牌表见 skill `worldengine-editor-ui` 的 references/design-tokens.md;
			// 结构体默认值就是暗色,这里显式返回一份,便于以后集中调参。
			return WuiTheme {};
		}

		WuiTheme MakeLightTheme()
		{
			WuiTheme theme;
			theme.WindowBg = { 0.949f, 0.957f, 0.969f, 1.0f };      // #F2F4F7
			theme.PanelBg = { 1.000f, 1.000f, 1.000f, 1.0f };       // #FFFFFF
			theme.PanelHeader = { 0.941f, 0.949f, 0.961f, 1.0f };   // #F0F2F5
			theme.ContentBg = { 0.980f, 0.984f, 0.992f, 1.0f };     // #FAFBFC
			theme.HoverBg = { 0.906f, 0.918f, 0.941f, 1.0f };       // #E7EAF0
			theme.ActiveBg = { 0.867f, 0.886f, 0.918f, 1.0f };      // #DDE2EA
			theme.Border = { 0.816f, 0.835f, 0.867f, 1.0f };        // #D0D5DD
			theme.BorderStrong = { 0.706f, 0.733f, 0.780f, 1.0f };  // #B4BBC7
			theme.Text = { 0.106f, 0.122f, 0.149f, 1.0f };          // #1B1F26
			theme.TextMuted = { 0.353f, 0.392f, 0.447f, 1.0f };     // #5A6472
			theme.TextDisabled = { 0.604f, 0.639f, 0.686f, 1.0f };  // #9AA3AF
			theme.Accent = { 0.184f, 0.435f, 0.922f, 1.0f };        // #2F6FEB
			theme.Success = { 0.106f, 0.620f, 0.259f, 1.0f };       // #1B9E42
			theme.Warning = { 0.706f, 0.478f, 0.031f, 1.0f };       // #B47A08
			theme.Danger = { 0.839f, 0.196f, 0.192f, 1.0f };        // #D63231
			theme.Selection = { 0.184f, 0.435f, 0.922f, 0.18f };
			theme.FocusRing = theme.Accent;
			theme.ButtonBg = theme.PanelHeader;
			theme.ButtonHover = theme.HoverBg;
			return theme;
		}

		bool SystemPrefersLightTheme()
		{
#if defined(_WIN32)
			// Windows 10/11 个性化设置:AppsUseLightTheme = 1 → 浅色。
			DWORD value = 0;
			DWORD size = sizeof(value);
			const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
				L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
				L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
			if (status == ERROR_SUCCESS)
				return value != 0;
#endif
			return false;   // 未知平台/读取失败:按暗色(引擎默认)处理。
		}

		const char* EnvString(const char* name)
		{
			const char* value = std::getenv(name);
			return value && value[0] ? value : nullptr;
		}

		WuiThemeMode ModeFromEnvironment()
		{
			if (const char* mode = EnvString("WLD_UI_THEME"))
			{
				if (std::strcmp(mode, "light") == 0)
					return WuiThemeMode::Light;
				if (std::strcmp(mode, "system") == 0)
					return WuiThemeMode::System;
				// 其余(dark / 未知)都按暗色。
			}
			return WuiThemeMode::Dark;
		}

		float ScaleFromEnvironment()
		{
			if (const char* text = EnvString("WLD_UI_SCALE"))
			{
				const float parsed = static_cast<float>(std::atof(text));
				if (parsed >= 0.5f && parsed <= 2.0f)
					return parsed;
				WLD_CORE_WARN("WLD_UI_SCALE 忽略(期望 0.5..2.0,收到 '{0}')", text);
			}
			// 默认 1.15:用户反馈"当前引擎字体偏小"。UI 缩放只改字,不改布局。
			return 1.15f;
		}

		struct ThemeState
		{
			WuiThemeMode Mode = WuiThemeMode::Dark;
			WuiTheme Theme {};
			uint32_t Generation = 0;
			float FontScale = 1.0f;
		};

		ThemeState& State()
		{
			static ThemeState state = [] {
				ThemeState out;
				out.Mode = ModeFromEnvironment();
				out.Theme = ResolveTheme(out.Mode);
				out.Generation = 1;
				out.FontScale = ScaleFromEnvironment();
				return out;
			}();
			return state;
		}
	}

	WuiTheme ResolveTheme(WuiThemeMode mode)
	{
		switch (mode)
		{
			case WuiThemeMode::Light:
				return MakeLightTheme();
			case WuiThemeMode::System:
				return SystemPrefersLightTheme() ? MakeLightTheme() : MakeDarkTheme();
			case WuiThemeMode::Dark:
			default:
				return MakeDarkTheme();
		}
	}

	const WuiTheme& CurrentTheme()
	{
		return State().Theme;
	}

	void SetThemeMode(WuiThemeMode mode)
	{
		ThemeState& state = State();
		if (state.Mode == mode)
			return;
		state.Mode = mode;
		state.Theme = ResolveTheme(mode);
		++state.Generation;
	}

	WuiThemeMode GetThemeMode()
	{
		return State().Mode;
	}

	uint32_t ThemeGeneration()
	{
		return State().Generation;
	}

	bool IsDarkTheme()
	{
		// 亮度判定:L 型相对亮度足够区分两套主题,避免再存一个布尔。
		const WuiColor& window = CurrentTheme().WindowBg;
		const float luminance = 0.2126f * window.R + 0.7152f * window.G + 0.0722f * window.B;
		return luminance < 0.5f;
	}

	float UiFontScale()
	{
		return State().FontScale;
	}

	void SetUiFontScale(float scale)
	{
		ThemeState& state = State();
		state.FontScale = std::clamp(scale, 0.5f, 2.0f);
	}
}
