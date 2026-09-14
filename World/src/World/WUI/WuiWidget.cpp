#include "wldpch.h"
#include "WuiWidget.h"

#include "World/WUI/WuiWidgets.h"

#include <algorithm>

namespace World::Wui
{
	// ---- WuiPaintContext ----

	void WuiPaintContext::PushClip(const WuiRect& rect)
	{
		m_ClipStack.push_back(m_Clip);
		const float x0 = std::max(m_Clip.X, rect.X);
		const float y0 = std::max(m_Clip.Y, rect.Y);
		const float x1 = std::min(m_Clip.X + m_Clip.W, rect.X + rect.W);
		const float y1 = std::min(m_Clip.Y + m_Clip.H, rect.Y + rect.H);
		m_Clip = { x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
		m_Context.Commands().push_back({ WuiDrawKind::ClipPush, m_Clip, { 0, 0, 0, 0 } });
	}

	void WuiPaintContext::PopClip()
	{
		if (!m_ClipStack.empty())
		{
			m_Clip = m_ClipStack.back();
			m_ClipStack.pop_back();
		}
		m_Context.Commands().push_back({ WuiDrawKind::ClipPop, m_Clip, { 0, 0, 0, 0 } });
	}

	// ---- WuiWidget ----

	WuiWidgetPtr WuiWidget::HitTest(glm::vec2 point)
	{
		return m_Rect.Contains(point) ? shared_from_this() : nullptr;
	}

	void WuiWidget::Invalidate()
	{
		const bool wasDirty = m_Dirty;
		m_Dirty = true;
		if (!wasDirty)
			if (const WuiWidgetPtr parent = m_Parent.lock())
			parent->Invalidate();
	}

	// ---- WuiBox ----

	void WuiBox::Add(const WuiWidgetPtr& child, const WuiFlexItem& item)
	{
		if (!child)
			return;
		child->SetParent(shared_from_this());
		m_Children.push_back({ child, item });
		Invalidate();
	}

	WuiMeasure WuiBox::Measure(const WuiConstraints& constraints)
	{
		if (!m_Dirty)
			return m_Measured;

		// 两遍测量:先收集子项主/交叉轴需求,再用 SolveFlex 求解主轴。
		std::vector<WuiFlexItem> items;
		items.reserve(m_Children.size());
		const bool row = Direction == WuiDirection::Row;
		const WuiConstraints childConstraints { 0, 0, constraints.MaxW, constraints.MaxH };
		for (const WuiFlexChild& child : m_Children)
		{
			const WuiMeasure measured = child.Widget->Measure(childConstraints);
			WuiFlexItem item = child.Item;
			const float intrinsicMain = row ? measured.Width : measured.Height;
			item.MinMain = intrinsicMain;
			// 契约把 MaxMain 当期望尺寸:默认无上限时用 intrinsic,
			// 避免非 grow 子项被容器可用空间撑满。
			item.MaxMain = child.Item.MaxMain > 1e29f ? intrinsicMain
				: std::max(child.Item.MaxMain, intrinsicMain);
			item.MinCross = row ? measured.Height : measured.Width;
			item.MaxCross = child.Item.MaxCross;
			items.push_back(item);
		}

		const WuiFlexLayout layout { Direction, AlignMain, AlignCross, Gap };
		const WuiFlexResult result = SolveFlex(layout, items, constraints);
		m_Measured = row
			? WuiMeasure { result.MainSize, result.CrossSize }
			: WuiMeasure { result.CrossSize, result.MainSize };
		MarkClean();
		return m_Measured;
	}

	void WuiBox::Arrange(const WuiRect& rect)
	{
		if (!m_Dirty && m_Rect.X == rect.X && m_Rect.Y == rect.Y && m_Rect.W == rect.W && m_Rect.H == rect.H)
			return;
		m_Rect = rect;
		WuiConstraints constraints { rect.W, rect.H, rect.W, rect.H };
		Measure(constraints);

		const bool row = Direction == WuiDirection::Row;
		std::vector<WuiFlexItem> items;
		items.reserve(m_Children.size());
		for (const WuiFlexChild& child : m_Children)
		{
			const WuiMeasure measured = child.Widget->Measure({ 0, 0, rect.W, rect.H });
			WuiFlexItem item = child.Item;
			const float intrinsicMain = row ? measured.Width : measured.Height;
			item.MinMain = intrinsicMain;
			item.MaxMain = child.Item.MaxMain > 1e29f ? intrinsicMain
				: std::max(child.Item.MaxMain, intrinsicMain);
			item.MinCross = row ? measured.Height : measured.Width;
			items.push_back(item);
		}
		const WuiFlexResult result = SolveFlex(
			{ Direction, AlignMain, AlignCross, Gap }, items, { rect.W, rect.H, rect.W, rect.H });

		for (size_t i = 0; i < m_Children.size(); ++i)
		{
			const WuiRect& childRect = result.Rects[i];
			WuiRect absolute = { rect.X + childRect.X, rect.Y + childRect.Y, childRect.W, childRect.H };
			// 交叉轴拉伸:容器 align 为 Stretch 时填满交叉方向。
			if (AlignCross == WuiAlign::Stretch)
			{
				if (row) absolute.H = rect.H;
				else absolute.W = rect.W;
			}
			m_Children[i].Widget->Arrange(absolute);
		}
		MarkClean();
	}

	void WuiBox::Paint(WuiPaintContext& context)
	{
		context.PushClip(m_Rect);
		for (const WuiFlexChild& child : m_Children)
			child.Widget->Paint(context);
		context.PopClip();
	}

	WuiWidgetPtr WuiBox::HitTest(glm::vec2 point)
	{
		if (!m_Rect.Contains(point))
			return nullptr;
		for (auto it = m_Children.rbegin(); it != m_Children.rend(); ++it)
			if (WuiWidgetPtr hit = it->Widget->HitTest(point))
				return hit;
		return shared_from_this();
	}

	// ---- WuiLabel ----

	WuiMeasure WuiLabel::Measure(const WuiConstraints& constraints)
	{
		// 无字体度量的无头环境:优先固定尺寸,否则取约束下限。
		const float width = FixedWidth >= 0 ? FixedWidth : constraints.MinW;
		const float height = FixedHeight >= 0 ? FixedHeight : constraints.MinH;
		MarkClean();
		return { width, height };
	}

	void WuiLabel::Paint(WuiPaintContext& context)
	{
		WuiDrawCommand command { WuiDrawKind::Text, { m_Rect.X, m_Rect.Y + std::max(0.0f, (m_Rect.H - FontSize) * 0.5f), m_Rect.W, FontSize },
			Color, 0, 1.0f, Text, FontSize, Bold };
		context.Context().Commands().push_back(std::move(command));
	}

	// ---- WuiButton ----

	WuiMeasure WuiButton::Measure(const WuiConstraints& constraints)
	{
		const auto clampSize = [](float value, float minValue, float maxValue)
		{
			return std::max(minValue, std::min(value, maxValue));
		};
		MarkClean();
		return { clampSize(80.0f, constraints.MinW, constraints.MaxW), clampSize(24.0f, constraints.MinH, constraints.MaxH) };
	}

	void WuiButton::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const bool hovered = ctx.IsHovered(m_Rect);
		const bool pressed = hovered && ctx.Input().MouseDown[0];
		ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, pressed ? WuiColor { 0.16f, 0.16f, 0.17f, 1 } : WuiColor { 0.2f, 0.21f, 0.23f, 1 }, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, m_Rect, WuiColor { 0.3f, 0.5f, 0.9f, 0.8f }, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { m_Rect.X + 8, m_Rect.Y + (m_Rect.H - 15.0f) * 0.5f, 0, 0 }, WuiColor { 0.82f, 0.84f, 0.87f, 1 }, 0, 1.0f, Label, 15.0f, false });
		if (ctx.IsClicked(m_Rect) && OnClick)
			OnClick();
	}

	// ---- WuiCheckbox ----

	WuiMeasure WuiCheckbox::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { std::max(constraints.MinW, std::min(24.0f, constraints.MaxW)),
			std::max(constraints.MinH, std::min(22.0f, constraints.MaxH)) };
	}

	void WuiCheckbox::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const WuiRect box { m_Rect.X, m_Rect.Y + (m_Rect.H - 18.0f) * 0.5f, 18, 18 };
		const bool value = Value ? *Value : false;
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? WuiColor { 0.3f, 0.5f, 0.9f, 1 } : WuiColor { 0.2f, 0.21f, 0.23f, 1 }, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, WuiColor { 0.3f, 0.3f, 0.32f, 1 }, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { box.X + 24, m_Rect.Y + (m_Rect.H - 15.0f) * 0.5f, 0, 0 }, WuiColor { 0.82f, 0.84f, 0.87f, 1 }, 0, 1.0f, Label, 15.0f, false });
		if (Value && ctx.IsClicked(m_Rect))
			*Value = !*Value;
	}

	// ---- WuiImage ----

	void WuiImage::Paint(WuiPaintContext& context)
	{
		context.Context().Commands().push_back({ WuiDrawKind::Image, m_Rect, Tint, 0, 1.0f, "", 15.0f, false, TextureId, Uv });
	}

	// ---- WuiTextField ----

	WuiMeasure WuiTextField::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { std::max(constraints.MinW, std::min(120.0f, constraints.MaxW)),
			std::max(constraints.MinH, std::min(24.0f, constraints.MaxH)) };
	}

	void WuiTextField::Paint(WuiPaintContext& context)
	{
		if (!Buffer)
			return;
		bool cancelled = false;
		if (TextField(context.Context(), Id(), m_Rect, *Buffer, Theme ? *Theme : WuiDefaultTheme(), &cancelled))
		{
			if (OnCommit) OnCommit();
		}
		else if (cancelled)
		{
			if (OnCancel) OnCancel();
		}
	}

	// ---- WuiDragFloat ----

	WuiMeasure WuiDragFloat::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { std::max(constraints.MinW, std::min(80.0f, constraints.MaxW)),
			std::max(constraints.MinH, std::min(22.0f, constraints.MaxH)) };
	}

	void WuiDragFloat::Paint(WuiPaintContext& context)
	{
		if (!Value)
			return;
		DragFloat(context.Context(), Id(), m_Rect, *Value, Speed, Min, Max, Theme ? *Theme : WuiDefaultTheme());
	}

	// ---- WuiDragInt ----

	WuiMeasure WuiDragInt::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { std::max(constraints.MinW, std::min(80.0f, constraints.MaxW)),
			std::max(constraints.MinH, std::min(22.0f, constraints.MaxH)) };
	}

	void WuiDragInt::Paint(WuiPaintContext& context)
	{
		if (!Value)
			return;
		DragInt(context.Context(), Id(), m_Rect, *Value, Min, Max, Theme ? *Theme : WuiDefaultTheme());
	}

	// ---- WuiCombo ----

	WuiMeasure WuiCombo::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { std::max(constraints.MinW, std::min(120.0f, constraints.MaxW)),
			std::max(constraints.MinH, std::min(24.0f, constraints.MaxH)) };
	}

	void WuiCombo::Paint(WuiPaintContext& context)
	{
		std::vector<std::string> empty;
		const std::vector<std::string>& options = Options ? *Options : empty;
		int selected = Selected ? *Selected : -1;
		Combo(context.Context(), Id(), m_Rect, Label, options, selected, Theme ? *Theme : WuiDefaultTheme());
		if (Selected)
			*Selected = selected;
	}

	// ---- WuiScrollArea ----

	WuiMeasure WuiScrollArea::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { constraints.MinW, constraints.MinH };
	}

	void WuiScrollArea::Arrange(const WuiRect& rect)
	{
		m_Rect = rect;
		if (Child)
			Child->Arrange({ rect.X, rect.Y - m_ScrollY, rect.W, std::max(rect.H, ContentHeight) });
		MarkClean();
	}

	void WuiScrollArea::Paint(WuiPaintContext& context)
	{
		float scrollY = m_ScrollY;
		BeginScrollArea(context.Context(), m_Rect, ContentHeight, scrollY, Theme ? *Theme : WuiDefaultTheme());
		if (Child)
		{
			Child->Arrange({ m_Rect.X, m_Rect.Y - scrollY, m_Rect.W, std::max(m_Rect.H, ContentHeight) });
			Child->Paint(context);
		}
		EndScrollArea(context.Context());
		m_ScrollY = scrollY;
	}

	WuiWidgetPtr WuiScrollArea::HitTest(glm::vec2 point)
	{
		if (!m_Rect.Contains(point))
			return nullptr;
		if (Child)
			if (WuiWidgetPtr hit = Child->HitTest(point))
				return hit;
		return shared_from_this();
	}

	// ---- 便捷布局 ----

	void LayoutWidgetTree(const WuiWidgetPtr& root, const WuiRect& rect)
	{
		if (!root)
			return;
		root->Arrange(rect);
	}
}
