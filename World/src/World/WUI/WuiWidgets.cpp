#include "wldpch.h"
#include "World/WUI/WuiWidgets.h"

#include <algorithm>

namespace World::Wui
{
	namespace
	{
		uint32_t ToImColor(const WuiColor& color)
		{
			auto clamp = [](float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); };
			const uint32_t r = static_cast<uint32_t>(clamp(color.R) * 255.0f + 0.5f);
			const uint32_t g = static_cast<uint32_t>(clamp(color.G) * 255.0f + 0.5f);
			const uint32_t b = static_cast<uint32_t>(clamp(color.B) * 255.0f + 0.5f);
			const uint32_t a = static_cast<uint32_t>(clamp(color.A) * 255.0f + 0.5f);
			return (a << 24) | (b << 16) | (g << 8) | r;
		}
	}

	void Panel(WuiContext& ctx, const WuiRect& rect, const std::string& title, const WuiTheme& theme)
	{
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.PanelBg, 4.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 4.0f, 1.0f });
		const WuiRect header { rect.X, rect.Y, rect.W, 26.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, header, theme.PanelHeader, 4.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { header.X + 10.0f, header.Y + 4.0f, 0, 0 }, theme.Text, 0, 1.0f, title, 15.0f, false });
	}

	void Label(WuiContext& ctx, const glm::vec2& pos, const std::string& text, const WuiColor& color, float fontSize)
	{
		ctx.Commands().push_back({ WuiDrawKind::Text, { pos.x, pos.y, 0, 0 }, color, 0, 1.0f, text, fontSize, false });
	}

	bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme)
	{
		const bool hovered = ctx.IsHovered(rect);
		const bool pressed = ctx.Input().MouseDown[0] && hovered;
		const WuiColor fill = hovered ? theme.ButtonHover : theme.ButtonBg;
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, fill, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, pressed ? theme.Accent : theme.Text, 0, 1.0f, label, 15.0f, false });
		(void)id;
		return hovered && ctx.Input().MouseClicked[0];
	}

	bool Toggle(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme)
	{
		bool& value = ctx.Persist<bool>(id, false);
		if (ctx.IsClicked(rect))
			value = !value;

		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, label, 15.0f, false });
		return value;
	}

	void SliderFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme)
	{
		const float range = std::max(0.0001f, max - min);
		if (ctx.Input().MouseDown[0] && ctx.IsHovered(rect))
		{
			const float fraction = (ctx.Input().MousePos.x - rect.X) / std::max(1.0f, rect.W);
			value = min + std::max(0.0f, std::min(1.0f, fraction)) * range;
		}

		const float trackH = 4.0f;
		const float trackY = rect.Y + rect.H * 0.5f - trackH * 0.5f;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, trackY, rect.W, trackH }, theme.ButtonBg, 2.0f });
		const float fraction = (value - min) / range;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, trackY, rect.W * fraction, trackH }, theme.Accent, 2.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X + rect.W * fraction - 4.0f, rect.Y + (rect.H - 12.0f) * 0.5f, 8.0f, 12.0f }, theme.Text, 2.0f });
		(void)id;
	}

	void TextField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer, const WuiTheme& theme)
	{
		if (ctx.IsClicked(rect))
			ctx.SetFocus(id);
		const bool focused = ctx.Focus() == id;
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, focused ? theme.Accent : theme.Border, 3.0f, focused ? 1.5f : 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, buffer, 15.0f, false });
		(void)focused;
	}
}
