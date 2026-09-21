#include "wldpch.h"
#include "WuiWidget.h"

#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/Core/KeyCodes.h"

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

	// ---- 对象式控件焦点(U2e)----

	void WuiWidget::RegisterFocusable(WuiPaintContext& context)
	{
		if (m_Id == 0)
			return;
		context.Context().RegisterFocusable(m_Id, m_Rect);
	}

	void WuiWidget::PaintFocusRing(WuiPaintContext& context)
	{
		if (m_Id == 0)
			return;
		// 与立即模式控件同一套语义:环画在 overlay 层、滚动裁剪区外不画。
		Wui::DrawFocusRing(context.Context(), m_Rect, m_Id, Wui::CurrentTheme());
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

	void WuiBox::Clear()
	{
		if (m_Children.empty())
			return;
		m_Children.clear();
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
		// flex 子项按无界约束测 intrinsic;容器空间由 SolveFlex 再分配。
		const WuiConstraints childConstraints { 0, 0, 1e30f, 1e30f };
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
			const WuiMeasure measured = child.Widget->Measure({ 0, 0, 1e30f, 1e30f });
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
		const float width = FixedWidth >= 0 ? FixedWidth : constraints.MinW;
		const float height = FixedHeight >= 0 ? FixedHeight : std::max(constraints.MinH, FontSize);
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
		// U2e:对象式按钮接入 P4-UX6 焦点体系(Tab 顺序 + 焦点环 + 键盘激活),
		// 与 Wui::Button 的立即模式实现同语义 —— 以前只有立即模式控件能被 Tab 到。
		RegisterFocusable(context);
		const bool focused = Id() != 0 && ctx.Focus() == Id();
		// P4-UX13:按钮样式统一走主题令牌 —— 以前这里是硬编码色 + **每颗按钮都描一圈蓝边**,
		// 工具条看起来像一排"空盒子"(用户:"顶部一系列按钮" 就是它)。
		const WuiTheme& theme = CurrentTheme();
		const bool hovered = ctx.IsHovered(m_Rect);
		const bool pressed = hovered && ctx.Input().MouseDown[0];
		// 键盘激活:焦点在本按钮上时 Enter / Space = 点击一次(KeyPressed 只含本帧新按下,长按不连发)。
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		const WuiColor fill = !Enabled ? theme.ContentBg
			: (pressed ? theme.ActiveBg : (hovered ? theme.ButtonHover : theme.ButtonBg));
		const WuiColor text = Enabled ? theme.Text : theme.TextDisabled;
		ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, fill, theme.Radius });
		// 只在悬停/按下时给一圈边:默认态干净,悬停态才有"可点"的反馈。
		if (Enabled && (hovered || pressed || focused))
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, m_Rect, theme.BorderStrong, theme.Radius, 1.0f });
		float textX = m_Rect.X + 8.0f;
		if (CenterLabel)
		{
			const float width = ctx.MeasureTextWidth(Label, 15.0f);
			textX = m_Rect.X + std::max(2.0f, (m_Rect.W - width) * 0.5f);
		}
		ctx.Commands().push_back({ WuiDrawKind::Text, { textX, m_Rect.Y + (m_Rect.H - 15.0f) * 0.5f, 0, 0 },
			text, 0, 1.0f, Label, 15.0f, false });
		PaintFocusRing(context);
		if (Enabled && hovered)
			ctx.SetCursor(WuiCursor::Hand);
		if (Enabled && (ctx.IsClicked(m_Rect) || keyActivated) && OnClick)
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
		// U2e:焦点登记 + 焦点环 + Enter/Space 切换(对象式勾选框此前完全不吃键盘)。
		RegisterFocusable(context);
		const bool focused = Id() != 0 && ctx.Focus() == Id();
		const bool keyToggled = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		const WuiRect box { m_Rect.X, m_Rect.Y + (m_Rect.H - 18.0f) * 0.5f, 18, 18 };
		const bool value = Value ? *Value : false;
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? WuiColor { 0.3f, 0.5f, 0.9f, 1 } : WuiColor { 0.2f, 0.21f, 0.23f, 1 }, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, WuiColor { 0.3f, 0.3f, 0.32f, 1 }, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { box.X + 24, m_Rect.Y + (m_Rect.H - 15.0f) * 0.5f, 0, 0 }, WuiColor { 0.82f, 0.84f, 0.87f, 1 }, 0, 1.0f, Label, 15.0f, false });
		PaintFocusRing(context);
		if (ctx.IsHovered(m_Rect))
			ctx.SetCursor(WuiCursor::Hand);
		if (Value && (ctx.IsClicked(m_Rect) || keyToggled))
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
		// U2e:输入框同样进焦点表(以前对象式输入框只能靠鼠标点,Tab 到不了)。
		// Enter 提交 / Escape 取消沿用立即模式 TextField 的既有实现,这里不重复一套。
		RegisterFocusable(context);
		bool cancelled = false;
		TextFieldA11y a11y;
		a11y.Label = A11yLabel;
		a11y.Placeholder = A11yPlaceholder;
		const TextFieldA11y* a11yPtr = (A11yLabel.empty() && A11yPlaceholder.empty()) ? nullptr : &a11y;
		if (TextField(context.Context(), Id(), m_Rect, *Buffer, Theme ? *Theme : WuiDefaultTheme(),
			&cancelled, a11yPtr))
		{
			if (OnCommit) OnCommit();
		}
		else if (cancelled)
		{
			if (OnCancel) OnCancel();
		}
		PaintFocusRing(context);
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
		RegisterFocusable(context);
		DragFloat(context.Context(), Id(), m_Rect, *Value, Speed, Min, Max, Theme ? *Theme : WuiDefaultTheme());
		PaintFocusRing(context);
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
		RegisterFocusable(context);
		DragInt(context.Context(), Id(), m_Rect, *Value, Min, Max, Theme ? *Theme : WuiDefaultTheme());
		PaintFocusRing(context);
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

	// ---- WuiProgress ----

	WuiMeasure WuiProgress::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { std::max(constraints.MinW, 8.0f), std::max(constraints.MinH, 8.0f) };
	}

	void WuiProgress::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const float clamped = std::max(0.0f, std::min(1.0f, Fraction));
		ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, TrackColor, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect,
			{ m_Rect.X, m_Rect.Y, m_Rect.W * clamped, m_Rect.H }, FillColor, 3.0f });
	}

	// ---- WuiListRow ----

	WuiMeasure WuiListRow::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { constraints.MinW, std::max(constraints.MinH, FontSize + 6.0f) };
	}

	void WuiListRow::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const bool hovered = ctx.IsHovered(m_Rect);
		if (Selected)
			ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, SelectedFill, 2.0f });
		else if (hovered)
			ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, HoverFill, 2.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text,
			{ m_Rect.X + 6 + Indent, m_Rect.Y + (m_Rect.H - FontSize) * 0.5f, 0, 0 },
			WuiColor { 0.82f, 0.84f, 0.87f, 1 }, 0, 1.0f, Text, FontSize, false });
		// P4-U5a:行文本必须进无障碍树 —— 否则"看得见、读不到也点不到"(层级面板实测)。
		if (m_Id != 0)
		{
			WuiAccessNode node;
			node.Id = m_Id;
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Kind = "list-row";
			node.Label = Text;
			node.Value = AccessValue;
			node.Tooltip = AccessTooltip;
			node.Rect = m_Rect;
			WuiAccessibility::Get().Register(node);
		}
		if (ctx.IsClicked(m_Rect) && OnClick)
			OnClick();
	}

	// ---- WuiImageButton ----

	WuiMeasure WuiImageButton::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { constraints.MinW, constraints.MinH };
	}

	void WuiImageButton::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		if (ctx.IsHovered(m_Rect))
			ctx.Commands().push_back({ WuiDrawKind::Rect, m_Rect, WuiColor { 1, 1, 1, 0.08f }, 2.0f });
		if (TextureId && !Dim)
			ctx.Commands().push_back({ WuiDrawKind::Image, m_Rect, WuiColor { 1, 1, 1, 1 }, 0, 1.0f, "", 15.0f, false, TextureId, Uv });
		else if (TextureId)
			ctx.Commands().push_back({ WuiDrawKind::Image, m_Rect, WuiColor { 1, 1, 1, 0.35f }, 0, 1.0f, "", 15.0f, false, TextureId, Uv });
		if (!Dim && ctx.IsClicked(m_Rect) && OnClick)
			OnClick();
	}

	// ---- WuiSection ----

	WuiMeasure WuiSection::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { constraints.MinW, 24.0f + (Open ? ContentHeight : 0.0f) };
	}

	void WuiSection::Arrange(const WuiRect& rect)
	{
		m_Rect = rect;
		m_ContentRect = { rect.X + 10, rect.Y + 24, rect.W - 10, ContentHeight };
		MarkClean();
	}

	void WuiSection::Paint(WuiPaintContext& context)
	{
		WuiContext& ctx = context.Context();
		const WuiRect header { m_Rect.X, m_Rect.Y, m_Rect.W, 24 };
		ctx.Commands().push_back({ WuiDrawKind::Rect, header, Open ? WuiColor { 0.27f, 0.28f, 0.31f, 1 } : HeaderFill, 2.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text,
			{ header.X + 6, header.Y + 3, 0, 0 },
			WuiColor { 0.82f, 0.84f, 0.87f, 1 }, 0, 1.0f, (Open ? "- " : "+ ") + Title, 14.0f, false });
		if (ctx.IsClicked(header))
		{
			Open = !Open;
			Invalidate();
		}
		if (Open && DrawContent)
			DrawContent(ctx, m_ContentRect);
	}

	WuiWidgetPtr WuiSection::HitTest(glm::vec2 point)
	{
		return m_Rect.Contains(point) ? shared_from_this() : nullptr;
	}

	// ---- WuiCustom ----

	WuiMeasure WuiCustom::Measure(const WuiConstraints& constraints)
	{
		MarkClean();
		return { constraints.MinW, std::max(constraints.MinH, ContentHeight) };
	}

	void WuiCustom::Paint(WuiPaintContext& context)
	{
		if (Draw)
			Draw(context.Context(), m_Rect);
	}

	// ---- 便捷布局 ----

	void LayoutWidgetTree(const WuiWidgetPtr& root, const WuiRect& rect)
	{
		if (!root)
			return;
		root->Arrange(rect);
	}
}
