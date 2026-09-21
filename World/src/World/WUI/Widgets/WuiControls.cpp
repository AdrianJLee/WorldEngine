#include "wldpch.h"
#include "WuiControls.h"

#include "World/WUI/WuiWidgets.h"

#include <algorithm>
#include <cstdio>

namespace World::Wui
{
	namespace
	{
		const WuiTheme& ThemeOf(const WuiTheme* theme)
		{
			return theme ? *theme : WuiDefaultTheme();
		}

		float ClampValue(float value, float minValue, float maxValue)
		{
			return std::max(minValue, std::min(value, maxValue));
		}

		void PushText(WuiContext& ctx, const WuiRect& rect, const std::string& text, const WuiColor& color, float fontSize, bool bold = false)
		{
			ctx.Commands().push_back({ WuiDrawKind::Text, rect, color, 0.0f, 1.0f, text, fontSize, bold });
		}
	}

	// ---- WuiSeparator ----

	WuiMeasure WuiSeparator::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { constraints.MinW, std::max(Thickness, constraints.MinH) };
	}

	void WuiSeparator::Paint(WuiPaintContext& context)
	{
		const WuiRect line { m_Rect.X, m_Rect.Y + (m_Rect.H - Thickness) * 0.5f, m_Rect.W, Thickness };
		context.Context().Commands().push_back({ WuiDrawKind::Rect, line, ThemeOf(Theme).Border, 0.0f });
	}

	// ---- WuiIconButton ----

	WuiMeasure WuiIconButton::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		const float size = std::max(IconSize + 8.0f, 24.0f);
		return { ClampValue(size, constraints.MinW, constraints.MaxW), ClampValue(size, constraints.MinH, constraints.MaxH) };
	}

	void WuiIconButton::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const WuiTheme& theme = ThemeOf(Theme);

		if (Hovered(ctx))
			ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, theme.ButtonHover, 4.0f });
		else if (Active(ctx))
			ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, theme.ButtonBg, 4.0f });

		if (TextureId != 0)
		{
			const float x = m_Rect.X + (m_Rect.W - IconSize) * 0.5f;
			const float y = m_Rect.Y + (m_Rect.H - IconSize) * 0.5f;
			const WuiColor tint = Disabled ? WuiColor { 1, 1, 1, 0.4f } : WuiColor { 1, 1, 1, 1 };
			ctx.Commands().push_back({ WuiDrawKind::Image, { x, y, IconSize, IconSize }, tint, 0.0f, 1.0f, "", 15.0f, false, TextureId, Uv });
		}

		// Tooltip:悬停时在 Overlay 层显示,不参与布局。
		if (!Tooltip.empty() && Hovered(ctx))
		{
			const glm::vec2 mouse = ctx.Input().MousePos;
			const float width = 8.0f * static_cast<float>(Tooltip.size()) + 16.0f;
			const WuiRect panel { mouse.x + 12.0f, mouse.y + 18.0f, width, 22.0f };
			ctx.PushOverlay();
			DrawPanelSurface(ctx, panel, theme);
			PushText(ctx, { panel.X + 8.0f, panel.Y + 4.0f, 0, 0 }, Tooltip, theme.Text, 14.0f);
			ctx.PopOverlay();
		}

		if (Clicked(ctx) && OnClick)
			OnClick();
	}

	// ---- WuiToggle ----

	WuiMeasure WuiToggle::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { ClampValue(140.0f, constraints.MinW, constraints.MaxW), ClampValue(22.0f, constraints.MinH, constraints.MaxH) };
	}

	void WuiToggle::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const WuiTheme& theme = ThemeOf(Theme);
		const bool value = Value ? *Value : false;
		const WuiColor textColor = Disabled ? theme.TextMuted : theme.Text;

		PushText(ctx, { m_Rect.X, m_Rect.Y + (m_Rect.H - 15.0f) * 0.5f, 0, 0 }, Label, textColor, 15.0f);

		const float trackW = 34.0f;
		const float trackH = 16.0f;
		const WuiRect track { m_Rect.X + m_Rect.W - trackW, m_Rect.Y + (m_Rect.H - trackH) * 0.5f, trackW, trackH };
		const WuiColor trackColor = value ? (Disabled ? WuiColor { 0.3f, 0.4f, 0.6f, 1 } : theme.Accent) : theme.ButtonBg;
		ctx.Commands().push_back({ WuiDrawKind::Rect, track, trackColor, trackH * 0.5f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, track, theme.Border, trackH * 0.5f, 1.0f });

		const float knobSize = trackH - 4.0f;
		const float knobX = value ? track.X + track.W - knobSize - 2.0f : track.X + 2.0f;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { knobX, track.Y + 2.0f, knobSize, knobSize }, theme.Text, knobSize * 0.5f });

		if (Value && Clicked(ctx))
		{
			*Value = !*Value;
			if (OnChanged) OnChanged();
		}
	}

	// ---- WuiSlider ----

	WuiMeasure WuiSlider::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { ClampValue(160.0f, constraints.MinW, constraints.MaxW), ClampValue(22.0f, constraints.MinH, constraints.MaxH) };
	}

	void WuiSlider::Paint(WuiPaintContext& context)
	{
		if (!Value)
			return;

		WuiContext& ctx = context.Context();
		const WuiTheme& theme = ThemeOf(Theme);

		struct DragState
		{
			bool Active = false;
		};
		// 拖动状态必须独立于其他持久状态 id(见 wui-patterns)。
		const WuiId dragId = HashId("wui.slider.drag") ^ (Id() * 2654435761u);
		DragState& drag = ctx.Persist<DragState>(dragId, {});

		const float minValue = std::min(Min, Max);
		const float maxValue = std::max(Min, Max);
		const float range = (maxValue - minValue) != 0.0f ? (maxValue - minValue) : 1.0f;

		const float trackX = m_Rect.X + 2.0f;
		const float trackW = std::max(1.0f, m_Rect.W - 4.0f);
		const float trackH = 6.0f;
		const WuiRect track { trackX, m_Rect.Y + (m_Rect.H - trackH) * 0.5f, trackW, trackH };

		const auto valueFromMouse = [&](float mouseX)
		{
			const float t = ClampValue((mouseX - trackX) / trackW, 0.0f, 1.0f);
			return minValue + t * range;
		};

		if (!Disabled)
		{
			if (ctx.IsClicked(m_Rect))
				drag.Active = true;
			if (drag.Active)
			{
				if (ctx.Input().MouseDown[0])
				{
					const float next = valueFromMouse(ctx.Input().MousePos.x);
					if (next != *Value)
					{
						*Value = next;
						if (OnChanged) OnChanged();
					}
				}
				else
				{
					drag.Active = false;
				}
			}
			if (Hovered(ctx))
				ctx.SetCursor(WuiCursor::Hand);
		}

		const float t = ClampValue((*Value - minValue) / range, 0.0f, 1.0f);
		const WuiColor trackColor = theme.ButtonBg;
		const WuiColor fillColor = Disabled ? WuiColor { 0.35f, 0.4f, 0.5f, 1 } : theme.Accent;
		ctx.Commands().push_back({ WuiDrawKind::Rect, track, trackColor, trackH * 0.5f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, { track.X, track.Y, track.W * t, track.H }, fillColor, trackH * 0.5f });

		const float knobSize = 12.0f;
		const float knobX = track.X + track.W * t - knobSize * 0.5f;
		const WuiColor knobColor = drag.Active ? theme.Accent : theme.Text;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { knobX, m_Rect.Y + (m_Rect.H - knobSize) * 0.5f, knobSize, knobSize }, knobColor, knobSize * 0.5f });
	}

	// ---- WuiListItem ----

	WuiMeasure WuiListItem::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { constraints.MinW, ClampValue(22.0f, constraints.MinH, constraints.MaxH) };
	}

	void WuiListItem::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const WuiTheme& theme = ThemeOf(Theme);

		if (Selected)
			ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, WuiColor { theme.Accent.R, theme.Accent.G, theme.Accent.B, 0.35f }, 0.0f });
		else if (Hovered(ctx))
			ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, theme.ButtonHover, 0.0f });

		float textX = m_Rect.X + 6.0f;
		if (IconTexture != 0)
		{
			const float iconSize = 16.0f;
			ctx.Commands().push_back({ WuiDrawKind::Image,
				{ m_Rect.X + 4.0f, m_Rect.Y + (m_Rect.H - iconSize) * 0.5f, iconSize, iconSize },
				WuiColor { 1, 1, 1, 1 }, 0.0f, 1.0f, "", 15.0f, false, IconTexture, { 0, 0, 1, 1 } });
			textX = m_Rect.X + 24.0f;
		}

		PushText(ctx, { textX, m_Rect.Y + (m_Rect.H - 14.0f) * 0.5f, 0, 0 },
			Label, Disabled ? theme.TextMuted : theme.Text, 14.0f, Selected);

		if (Clicked(ctx))
		{
			if (OnSelect) OnSelect();
			if (OnActivate && ctx.IsDoubleClicked(m_Rect)) OnActivate();
		}
	}

	// ---- WuiTreeItem ----

	WuiMeasure WuiTreeItem::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { constraints.MinW, ClampValue(22.0f, constraints.MinH, constraints.MaxH) };
	}

	void WuiTreeItem::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const WuiTheme& theme = ThemeOf(Theme);

		if (Selected)
			ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, WuiColor { theme.Accent.R, theme.Accent.G, theme.Accent.B, 0.35f }, 0.0f });
		else if (Hovered(ctx))
			ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, theme.ButtonHover, 0.0f });

		const float indent = 8.0f + static_cast<float>(std::max(0, Depth)) * 14.0f;
		const WuiRect arrowRect { m_Rect.X + indent, m_Rect.Y + (m_Rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		if (!Leaf())
		{
			const std::string glyph = (*Expanded) ? "v" : ">";
			PushText(ctx, { arrowRect.X + 4.0f, arrowRect.Y + 1.0f, 0, 0 }, glyph, theme.TextMuted, 14.0f);
		}

		PushText(ctx, { arrowRect.X + 18.0f, m_Rect.Y + (m_Rect.H - 14.0f) * 0.5f, 0, 0 },
			Label, Disabled ? theme.TextMuted : theme.Text, 14.0f, Selected);

		if (Clicked(ctx))
		{
			if (!Leaf() && ctx.IsHovered(arrowRect))
			{
				*Expanded = !*Expanded;
				if (OnToggle) OnToggle();
			}
			else if (OnSelect)
			{
				OnSelect();
			}
		}
	}

	// ---- WuiTabs ----

	WuiMeasure WuiTabs::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { constraints.MinW, ClampValue(26.0f, constraints.MinH, constraints.MaxH) };
	}

	void WuiTabs::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const WuiTheme& theme = ThemeOf(Theme);
		if (Labels.empty())
			return;

		ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, theme.PanelHeader, 0.0f });

		const float cellW = m_Rect.W / static_cast<float>(Labels.size());
		const int active = Selected ? *Selected : 0;
		for (size_t i = 0; i < Labels.size(); ++i)
		{
			const WuiRect cell { m_Rect.X + cellW * static_cast<float>(i), m_Rect.Y, cellW, m_Rect.H };
			const bool isActive = static_cast<int>(i) == active;
			if (isActive)
			{
				ctx.Commands().push_back({ WuiDrawKind::Rect, cell, theme.PanelBg, 0.0f });
				ctx.Commands().push_back({ WuiDrawKind::Rect, { cell.X, cell.Y + cell.H - 2.0f, cell.W, 2.0f }, theme.Accent, 0.0f });
			}
			else if (Hovered(ctx) && ctx.IsHovered(cell))
			{
				ctx.Commands().push_back({ WuiDrawKind::Rect, cell, theme.ButtonHover, 0.0f });
			}
			PushText(ctx, { cell.X + 10.0f, cell.Y + (cell.H - 15.0f) * 0.5f, 0, 0 },
				Labels[i], isActive ? theme.Text : theme.TextMuted, 15.0f, isActive);
		}

		if (!Disabled && Selected && ctx.IsClicked(m_Rect))
		{
			const int index = static_cast<int>(ClampValue((ctx.Input().MousePos.x - m_Rect.X) / std::max(1.0f, cellW), 0.0f,
				static_cast<float>(Labels.size() - 1)));
			if (index != *Selected)
			{
				*Selected = index;
				if (OnChanged) OnChanged(index);
			}
		}
	}

	// ---- WuiTooltip ----

	void WuiTooltip::Paint(WuiPaintContext& context)
	{
		if (Text.empty())
			return;
		WuiContext& ctx = context.Context();
		if (!ctx.IsHovered(Anchor))
			return;

		const WuiTheme& theme = ThemeOf(Theme);
		const glm::vec2 mouse = ctx.Input().MousePos;
		const glm::vec2 viewport = ctx.ViewportSize();
		const float width = 8.0f * static_cast<float>(Text.size()) + 16.0f;
		float x = mouse.x + 12.0f;
		float y = mouse.y + 18.0f;
		if (x + width > viewport.x)
			x = std::max(0.0f, viewport.x - width - 4.0f);
		if (y + 22.0f > viewport.y)
			y = std::max(0.0f, mouse.y - 26.0f);

		const WuiRect panel { x, y, width, 22.0f };
		ctx.PushOverlay();
		DrawPanelSurface(ctx, panel, theme);
		PushText(ctx, { panel.X + 8.0f, panel.Y + 4.0f, 0, 0 }, Text, theme.Text, 14.0f);
		ctx.PopOverlay();
	}

	// ---- WuiMenuButton ----

	WuiMeasure WuiMenuButton::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { ClampValue(110.0f, constraints.MinW, constraints.MaxW), ClampValue(24.0f, constraints.MinH, constraints.MaxH) };
	}

	void WuiMenuButton::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const WuiTheme& theme = ThemeOf(Theme);
		const WuiId popupId = Id() != 0 ? Id() : HashId("wui.menubutton");

		const bool open = ctx.IsPopupOpen(popupId);
		const WuiColor buttonColor = (open || Active(ctx)) ? theme.ButtonHover : theme.ButtonBg;
		ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, buttonColor, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, m_Rect, theme.Border, 3.0f, 1.0f });
		PushText(ctx, { m_Rect.X + 8.0f, m_Rect.Y + (m_Rect.H - 15.0f) * 0.5f, 0, 0 }, Label, theme.Text, 15.0f);

		if (Clicked(ctx))
		{
			if (open) ctx.ClosePopup(popupId);
			else ctx.OpenPopup(popupId);
		}

		if (!open || Items.empty())
			return;

		const float itemH = 22.0f;
		const float panelW = std::max(m_Rect.W, 150.0f);
		const WuiRect panel { m_Rect.X, m_Rect.Y + m_Rect.H + 2.0f, panelW, itemH * static_cast<float>(Items.size()) + 8.0f };

		ctx.PushOverlay();
		DrawPanelSurface(ctx, panel, theme);
		// P4-U7:登记覆盖层矩形 → 下一帧面板内容不会吃掉落在菜单上的点击。
		ctx.RegisterOverlayRect(panel);
		int chosen = -1;
		for (size_t i = 0; i < Items.size(); ++i)
		{
			const WuiRect item { panel.X + 4.0f, panel.Y + 4.0f + itemH * static_cast<float>(i), panel.W - 8.0f, itemH };
			if (MenuItem(ctx, HashId("wui.menubutton.item") + static_cast<WuiId>(i), item, Items[i], true, theme))
				chosen = static_cast<int>(i);
		}
		ctx.PopOverlay();

		if (chosen >= 0)
		{
			ctx.ClosePopup(popupId);
			if (OnSelect) OnSelect(chosen);
		}
		else
		{
			// 点击菜单以外的区域关闭(位置在打开时固定,避免菜单跟随鼠标)。
			ctx.ClosePopupsOnOutsideClick({ popupId }, panel);
		}
	}
}
