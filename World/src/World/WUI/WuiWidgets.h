#pragma once

#include "World/WUI/WuiContext.h"

namespace World::Wui
{
	// 默认主题;应用可覆盖后传入控件。
	struct WuiTheme
	{
		WuiColor PanelBg { 0.12f, 0.125f, 0.13f, 1.0f };
		WuiColor PanelHeader { 0.16f, 0.165f, 0.17f, 1.0f };
		WuiColor Border { 0.25f, 0.26f, 0.28f, 1.0f };
		WuiColor Text { 0.82f, 0.84f, 0.87f, 1.0f };
		WuiColor TextMuted { 0.55f, 0.58f, 0.62f, 1.0f };
		WuiColor ButtonBg { 0.20f, 0.21f, 0.23f, 1.0f };
		WuiColor ButtonHover { 0.27f, 0.28f, 0.31f, 1.0f };
		WuiColor Accent { 0.30f, 0.50f, 0.90f, 1.0f };
	};

	void Panel(WuiContext& ctx, const WuiRect& rect, const std::string& title, const WuiTheme& theme);
	void Label(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const WuiColor& color, float fontSize);
	bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);
	bool Toggle(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme);
	void SliderFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme);
	void TextField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme);
}
