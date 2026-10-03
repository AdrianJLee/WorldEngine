#include "WuiWidgets_Internal.h"

namespace World::Wui
{

using namespace WuiWidgetsDetail;

namespace WuiWidgetsDetail
{

		// 本帧的延后批次(按上下文持有;首帧/换帧时清空上一帧的残留)。
WuiDeferredPopupLayer& DeferredPopupLayer(WuiContext& ctx){
			WuiDeferredPopupLayer& layer = ctx.Persist<WuiDeferredPopupLayer>(
				HashId("world.wui.deferred-popup"), {});
			if (layer.Frame != ctx.Frame())
			{
				layer.Commands.clear();
				layer.Frame = ctx.Frame();
			}
			return layer;
		}


		// 帧末收口:把本帧攒下的弹层命令追加到 overlay 末尾。
void FlushDeferredPopupDraws(WuiContext& ctx){
			WuiDeferredPopupLayer& layer = DeferredPopupLayer(ctx);
			if (layer.Frame != ctx.Frame() || layer.Commands.empty())
				return;
			ctx.PushOverlay();
			std::vector<WuiDrawCommand>& target = ctx.Commands();
			for (WuiDrawCommand& command : layer.Commands)
				target.push_back(command);
			ctx.PopOverlay();
			layer.Commands.clear();
		}

}

void Tooltip(WuiContext& ctx, const WuiRect& hoverRect, const std::string& text){
		if (text.empty() || !ctx.IsHovered(hoverRect))
			return;
		ctx.SetTooltip(text);
	}


void DrawTooltip(WuiContext& ctx, const WuiTheme& theme){
		// P4-U29:先把本帧延后的下拉弹层补画到 overlay 末尾(在所有面板/模态内容之上),
		// 再画 tooltip —— tooltip 仍在最上层;没有 tooltip 时也必须走这一步。
		FlushDeferredPopupDraws(ctx);
		const std::string& text = ctx.Tooltip();
		if (text.empty())
			return;
		const float fontSize = theme.FontSizeSmall;
		const float padding = 8.0f;
		const float lineHeight = fontSize + 6.0f;
		const float maxTextWidth = 380.0f;
		const std::vector<std::string> lines = WrapTooltipText(ctx, text, fontSize, maxTextWidth);
		float textWidth = 0.0f;
		for (const std::string& line : lines)
			textWidth = std::max(textWidth, ctx.MeasureTextWidth(line, fontSize));
		const float height = static_cast<float>(lines.size()) * lineHeight + padding * 2.0f;
		const glm::vec2 viewport = ctx.ViewportSize();
		// 跟随光标右下 16/20,贴边时翻到另一侧,保证整块提示在视口内可见。
		glm::vec2 pos { ctx.Input().MousePos.x + 16.0f, ctx.Input().MousePos.y + 20.0f };
		const float panelWidth = textWidth + padding * 2.0f;
		if (pos.x + panelWidth > viewport.x - 4.0f)
			pos.x = std::max(4.0f, ctx.Input().MousePos.x - panelWidth - 12.0f);
		if (pos.y + height > viewport.y - 4.0f)
			pos.y = std::max(4.0f, ctx.Input().MousePos.y - height - 12.0f);
		const WuiRect panel { pos.x, pos.y, panelWidth, height };
		ctx.PushOverlay();
		DrawPanelSurface(ctx, panel, theme);
		float y = panel.Y + padding;
		for (const std::string& line : lines)
		{
			ctx.Commands().push_back({ WuiDrawKind::Text, { panel.X + padding, y, 0, 0 },
				theme.Text, 0, 1.0f, line, fontSize, false });
			y += lineHeight;
		}
		ctx.PopOverlay();
	}


void DrawFocusRing(WuiContext& ctx, const WuiRect& rect, WuiId id, const WuiTheme& theme, const WuiColor* ringColor){
		if (id == 0 || ctx.Focus() != id)
			return;
		// 滚出滚动区视口的控件不画环(overlay 不受 ClipPush 影响,必须自己问裁剪栈)。
		if (!ctx.ClipAllows(rect))
			return;
		// 走 overlay 命令层:焦点环在全部普通控件之后绘制,后画的兄弟控件不会盖住它
		// (与 tooltip 同一套机制,不新增层级)。
		//
		// MAT-UI3a(用户 2026-09-25「这个聚焦选中能否按照人类美学重新设计下」):从"1.5px 不透明
		// 强调色硬描边"改成"圆角细描边 + 外发光",两条都以基色为色相、按低透明度画:
		//   ① 主环:贴着控件矩形(不动几何),1.25px,圆角 = theme.Radius,alpha × 0.72;
		//   ② 发光:矩形外扩 1.5px,2.5px,圆角 = theme.Radius + 2.5,alpha × 0.16 —— 只添氛围,
		//      不占布局、不进命中、不动 a11y(环是纯绘制)。
		// 可辨识性优先:主环 alpha 0.72 + 1.25px 实线在明暗两套主题上都明确可辨(比旧口径低一档
		// 透明度,但描边更圆更细,视觉噪声小得多);禁用件仍走各自"不画环"的分支,与本函数无关。
		// 顺序:主环在前(投影/断言里它就是"这条焦点环"),发光在后(纯附加)。
		const WuiColor base = ringColor != nullptr ? *ringColor : theme.FocusRing;
		const auto scaled = [](const WuiColor& color, float alphaScale)
		{
			return WuiColor { color.R, color.G, color.B,
				std::clamp(color.A * alphaScale, 0.0f, 1.0f) };
		};
		ctx.PushOverlay();
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
			scaled(base, kFocusRingCoreAlpha), theme.Radius, kFocusRingCoreThickness });
		const float glow = kFocusRingGlowInset;
		ctx.Commands().push_back({ WuiDrawKind::RectOutline,
			{ rect.X - glow, rect.Y - glow, rect.W + glow * 2.0f, rect.H + glow * 2.0f },
			scaled(base, kFocusRingGlowAlpha), theme.Radius + 2.5f, kFocusRingGlowThickness });
		ctx.PopOverlay();
	}


	// WUI-P1.5a2:按"当前视觉状态 + 通道"取样式色 —— 保留模式 WuiButton 与立即模式 Button 的**唯一**实现。
	// 槽未覆盖 = 调用方给的兜底(主题令牌/旧口径);哨兵口径(字号<=0 / 内边距<0 / 空 optional)也在这里。
WuiButtonResolvedStyle ResolveButtonStyle(const WuiButtonStyle* style, bool disabled, bool pressed, bool hovered, bool focused, const WuiColor& fallbackBg, const WuiColor& fallbackBorder, const WuiColor& fallbackText, const WuiColor& fallbackFocusRing){
		const WuiButtonStyle::State state = disabled ? WuiButtonStyle::State::Disabled
			: (pressed ? WuiButtonStyle::State::Pressed
				: (hovered ? WuiButtonStyle::State::Hover
					: (focused ? WuiButtonStyle::State::Focused : WuiButtonStyle::State::Normal)));
		const WuiButtonStateColors* colors = style != nullptr
			? &style->Colors[static_cast<size_t>(state)] : nullptr;
		WuiButtonResolvedStyle resolved;
		resolved.State = state;
		resolved.BgCovered = colors != nullptr && colors->Bg.has_value();
		resolved.BorderCovered = colors != nullptr && colors->Border.has_value();
		resolved.TextCovered = colors != nullptr && colors->Text.has_value();
		resolved.Bg = resolved.BgCovered ? *colors->Bg : fallbackBg;
		resolved.Border = resolved.BorderCovered ? *colors->Border : fallbackBorder;
		resolved.Text = resolved.TextCovered ? *colors->Text : fallbackText;
		// border.focus 覆盖同时作用于焦点环(与当前状态无关);未覆盖 = 调用方给的主题 FocusRing。
		const std::optional<WuiColor>* ring = style != nullptr
			? &style->Colors[static_cast<size_t>(WuiButtonStyle::State::Focused)].Border : nullptr;
		resolved.FocusRingCovered = ring != nullptr && ring->has_value();
		resolved.FocusRing = resolved.FocusRingCovered ? **ring : fallbackFocusRing;
		resolved.PaddingX = style != nullptr && style->PaddingX >= 0.0f ? style->PaddingX : 8.0f;
		resolved.FontSize = style != nullptr && style->FontSize > 0.0f ? style->FontSize : 15.0f;
		resolved.Bold = style != nullptr && style->Bold;
		return resolved;
	}


bool Button(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme, const WuiButtonStyle* style){
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "button", rect, label, std::string(), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const bool hovered = ctx.IsHovered(rect);
		const bool pressed = ctx.Input().MouseDown[0] && hovered;
		// U2A 键盘激活:焦点在按钮上时 Enter/Space = 点击一次(KeyPressed 只含本帧新按下,长按不连发)。
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		// WUI-P1.5:状态优先级 pressed > hover > focus > normal,disabled 覆盖一切;全部槽都未覆盖时
		// 下面三个兜底就是改动前的取值(ButtonHover/ButtonBg、Border、pressed?Accent:Text)⇒ 命令流逐字节不变。
		const WuiButtonResolvedStyle resolved = ResolveButtonStyle(style, style != nullptr && style->Disabled,
			pressed, hovered, focused, hovered ? theme.ButtonHover : theme.ButtonBg,
			theme.Border, pressed ? theme.Accent : theme.Text, theme.FocusRing);
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, resolved.Bg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, resolved.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text,
			{ rect.X + resolved.PaddingX, rect.Y + (rect.H - resolved.FontSize) * 0.5f, 0, 0 },
			resolved.Text, 0, 1.0f, label, resolved.FontSize, resolved.Bold });
		DrawFocusRing(ctx, rect, id, theme, resolved.FocusRingCovered ? &resolved.FocusRing : nullptr);
		return (hovered && ctx.Input().MouseClicked[0]) || keyActivated;
	}


	// P1c-LIB2:带禁用态 + 理由的按钮 —— 画法与面板侧 ActionButton/ModalActionButton 同源,
	// 语义收进库:"不可用"必须同时体现在 Enabled 与 Value/Tooltip(禁用的理由)上。
	// P1c-LIB3(用户裁决 2026-09-24):enabled=false 时不登记焦点表 ⇒ 不进 Tab 焦点链;
	// a11y 节点照登记(Enabled=false + Value/Tooltip=理由),即"可读不可点"。
bool ButtonEx(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme, bool enabled, bool primary, const std::string& tooltip){
		const bool hovered = ctx.IsHovered(rect);
		const bool focused = ctx.Focus() == id;
		const WuiColor fill = !enabled
			? theme.PanelBg
			: (primary ? theme.Accent : (hovered ? theme.ButtonHover : theme.ButtonBg));
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, fill, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect,
			enabled ? (hovered ? theme.Accent : theme.Border) : theme.Border, 3.0f, 1.0f });
		// accent 填充上压深色文字(白字对比度不够);禁用态用 TextDisabled。
		const WuiColor textColor = !enabled ? theme.TextDisabled : (primary ? theme.WindowBg : theme.Text);
		ctx.Commands().push_back({ WuiDrawKind::Text,
			{ rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, textColor, 0, 1.0f, label, 15.0f, false });
		// 灰按钮不能没有理由:禁用时这一句进 Value 与 Tooltip(启用时是普通用途说明)。
		if (id != 0)
		{
			WuiAccessNode node;
			node.Id = id;
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Kind = "button";
			node.Label = label;
			node.Value = tooltip;
			node.Tooltip = tooltip;
			node.Rect = rect;
			node.Enabled = enabled;
			node.Interactive = true;
			node.Focused = focused;
			WuiAccessibility::Get().Register(node);
		}
		// P1c-LIB3:禁用件不进 Tab 焦点链(焦点表登记 = Tab/Shift+Tab 的唯一入口);
		// 启用件与普通 Button 完全同一条登记路径 —— 普通按钮/其它控件不受影响。
		if (enabled)
		{
			ctx.RegisterFocusable(id, rect);
			// 禁用件不画焦点环(即使调用方在同一帧强设焦点):"不可聚焦"必须在外观上一致。
			DrawFocusRing(ctx, rect, id, theme);
		}
		if (hovered && !tooltip.empty())
			Tooltip(ctx, rect, tooltip);
		if (enabled)
		{
			if (hovered)
				ctx.SetCursor(WuiCursor::Hand);
			const bool keyActivated = focused
				&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
			return (hovered && ctx.Input().MouseClicked[0]) || keyActivated;
		}
		return false;
	}


bool Toggle(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const WuiTheme& theme){
		bool& value = ctx.Persist<bool>(id, false);
		const bool focused = ctx.Focus() == id;
		// U2A 键盘激活:焦点在开关上时 Enter/Space = 切换。
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		if (ctx.IsClicked(rect) || keyActivated)
			value = !value;
		RegisterAccessNode(id, "toggle", rect, label, value ? "on" : "off", true, true, focused);
		ctx.RegisterFocusable(id, rect);

		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, label, 15.0f, false });
		DrawFocusRing(ctx, rect, id, theme);
		return value;
	}


bool TabBar(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::vector<std::string>& tabs, int& active, const WuiTheme& theme, int* closeRequested){
		if (closeRequested)
			*closeRequested = -1;
		if (id == 0 || tabs.empty())
			return false;
		if (active < 0 || active >= static_cast<int>(tabs.size()))
			active = 0;
		bool changed = false;
		const float tabW = rect.W / static_cast<float>(tabs.size());
		// 标签条本体的节点(P1c-a):标签等分整条矩形 → 节点矩形里任何一点都落在某个标签上,
		// 按它注入的点击(= 组中心那一格)一定激活一个真实标签;要精确点某一个仍用子节点
		// kind="tab"(派生 id)。
		// P1c-E4:焦点停在**某个标签**上(子 id),组节点按"组里有焦点"报 focused=true —— 这样
		// AI 既能按组 id 判"标签条整体有键盘焦点",也能从子节点读"焦点在第几个标签"。
		bool groupFocused = ctx.Focus() == id;
		for (size_t i = 0; i < tabs.size() && !groupFocused; ++i)
			groupFocused = ctx.Focus() == DerivedChildId(id, ".tab.", i);
		RegisterAccessNode(id, "tab-bar", rect, std::string(), tabs[static_cast<size_t>(active)], true, true,
			groupFocused);
		// 整条底边线:让标签条与下方内容有分界(取面板边框色,不新增样式常量)。
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, rect.Y + rect.H - 1.0f, rect.W, 1.0f }, theme.Border, 0.0f });
		for (size_t i = 0; i < tabs.size(); ++i)
		{
			const WuiRect tab { rect.X + tabW * static_cast<float>(i), rect.Y, tabW, rect.H };
			const WuiId tabId = DerivedChildId(id, ".tab.", i);
			const bool isActive = static_cast<int>(i) == active;
			const bool hovered = ctx.IsHovered(tab);
			// P1c-E4:子节点的 focused 实参以前缺省(false)→ "焦点在哪一页"AI 看不见。
			RegisterAccessNode(tabId, "tab", tab, tabs[i], isActive ? "true" : "false", true, true,
				ctx.Focus() == tabId);
			ctx.RegisterFocusable(tabId, tab);
			if (isActive)
				ctx.Commands().push_back({ WuiDrawKind::Rect, tab, theme.ActiveBg, 0.0f });
			else if (hovered)
				ctx.Commands().push_back({ WuiDrawKind::Rect, tab, theme.HoverBg, 0.0f });
			if (hovered)
				ctx.SetCursor(WuiCursor::Hand);
			const std::string text = EllipsizeToWidth(ctx, tabs[i], std::max(0.0f, tab.W - theme.Pad * 2.0f), theme.FontSizeBody);
			if (!text.empty())
				ctx.Commands().push_back({ WuiDrawKind::Text,
					{ tab.X + theme.Pad, tab.Y + (tab.H - theme.FontSizeBody) * 0.5f, 0, 0 },
					isActive ? theme.Text : theme.TextMuted, 0, 1.0f, text, theme.FontSizeBody, false });
			if (isActive)
			{
				// 活跃标签:底部 2px 强调色下划线。
				ctx.Commands().push_back({ WuiDrawKind::Rect,
					{ tab.X, tab.Y + tab.H - 2.0f, tab.W, 2.0f }, theme.Accent, 0.0f });
			}
			DrawFocusRing(ctx, tab, tabId, theme);
			const bool keyActivated = ctx.Focus() == tabId
				&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
			if (ctx.IsClicked(tab) || keyActivated)
			{
				if (!isActive)
				{
					active = static_cast<int>(i);
					changed = true;
				}
			}
			else if (closeRequested && ctx.IsClicked(tab, 2))
				*closeRequested = static_cast<int>(i);   // 中键:只上报请求,不自行关闭
		}
		return changed;
	}


bool Segmented(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::vector<std::string>& options, int& selected, const WuiTheme& theme){
		if (options.empty())
			return false;
		if (selected < 0 || selected >= static_cast<int>(options.size()))
			selected = 0;
		bool changed = false;
		// 组节点 + 逐个分段节点都登记(P1c-a):分段把整条矩形铺满,所以**组节点也是真的可点**——
		// 按组节点注入的点击落在组中心,命中的是那里那一格分段,与用户点击走同一条路径
		// (于是 interactive=true 不撒谎);要精确选某一格仍用 kind="segmented-option" 的子节点。
		// P1c-E4:焦点停在**某一格**上(子 id),组节点按"组里有焦点"报 focused=true —— AI 既能按组
		// id 判"分段控件有键盘焦点",也能从子节点读"焦点在第几格"。
		bool groupFocused = ctx.Focus() == id;
		for (size_t i = 0; i < options.size() && !groupFocused; ++i)
			groupFocused = ctx.Focus() == DerivedChildId(id, ".segment.", i);
		RegisterAccessNode(id, "segmented", rect, std::string(), options[static_cast<size_t>(selected)],
			true, true, groupFocused);
		// 同一圆角外壳:铺底 + 1px 描边,内部等分(选中项 ActiveBg + Accent 文本)。
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonBg, theme.Radius });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, theme.Radius, 1.0f });
		const float optionW = rect.W / static_cast<float>(options.size());
		for (size_t i = 0; i < options.size(); ++i)
		{
			const WuiRect option { rect.X + optionW * static_cast<float>(i), rect.Y, optionW, rect.H };
			const WuiId optionId = DerivedChildId(id, ".segment.", i);
			const bool isSelected = static_cast<int>(i) == selected;
			const bool hovered = ctx.IsHovered(option);
			// P1c-E4:子节点的 focused 实参以前缺省(false)→ "焦点在哪一格"AI 看不见。
			RegisterAccessNode(optionId, "segmented-option", option, options[i], isSelected ? "true" : "false",
				true, true, ctx.Focus() == optionId);
			ctx.RegisterFocusable(optionId, option);
			const WuiRect inner { option.X + 1.0f, option.Y + 1.0f,
				std::max(0.0f, option.W - 2.0f), std::max(0.0f, option.H - 2.0f) };
			if (isSelected)
				ctx.Commands().push_back({ WuiDrawKind::Rect, inner, theme.ActiveBg, std::max(0.0f, theme.Radius - 1.0f) });
			else if (hovered)
				ctx.Commands().push_back({ WuiDrawKind::Rect, inner, theme.HoverBg, std::max(0.0f, theme.Radius - 1.0f) });
			if (hovered)
				ctx.SetCursor(WuiCursor::Hand);
			const std::string text = EllipsizeToWidth(ctx, options[i], std::max(0.0f, option.W - theme.Pad * 2.0f), theme.FontSizeSmall);
			if (!text.empty())
				ctx.Commands().push_back({ WuiDrawKind::Text,
					{ option.X + theme.Pad, option.Y + (option.H - theme.FontSizeSmall) * 0.5f, 0, 0 },
					isSelected ? theme.Accent : theme.Text, 0, 1.0f, text, theme.FontSizeSmall, false });
			DrawFocusRing(ctx, option, optionId, theme);
			const bool keyActivated = ctx.Focus() == optionId
				&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
			if ((ctx.IsClicked(option) || keyActivated) && !isSelected)
			{
				selected = static_cast<int>(i);
				changed = true;
			}
		}
		return changed;
	}


bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool& value, const WuiTheme& theme){
		return Checkbox(ctx, id, rect, label, std::string(), value, theme);
	}


bool Checkbox(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, const std::string& term, bool& value, const WuiTheme& theme){
		// 返回"本帧是否被点击改值",与 Combo/DragInt/DragFloat 的约定一致(旧版返回 value,
		// 导致 `if (Checkbox(...))` 的调用点在**勾选时每帧触发、取消勾选时反而不触发**)。
		// 控件状态本身仍然写回 value 引用,并在无障碍节点里记录。
		bool changed = false;
		const bool focused = ctx.Focus() == id;
		// U2A 键盘激活:焦点在勾选框上时 Enter/Space = 切换。
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		if (ctx.IsClicked(rect) || keyActivated)
		{
			value = !value;
			changed = true;
		}
		// 无障碍节点带上英文术语:脚本/自动化在中文界面下也能按英文检索。
		RegisterAccessNode(id, "checkbox", rect,
			term.empty() ? label : (label + " (" + term + ")"), value ? "true" : "false", true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, value ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		const float labelSize = 15.0f;
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - labelSize) * 0.5f, 0, 0 },
			theme.Text, 0, 1.0f, label, labelSize, false });
		if (!term.empty())
		{
			// 英文术语对照:Caption/次要色。放不下时与 LabelWithTerm 同族降级 —— 先按可用宽度
			// 省略号截断(而不是整段不画);连一个字符加省略号都放不下才不画。
			const float termSize = theme.FontSizeCaption;
			const float termX = rect.X + 24.0f + ctx.MeasureTextWidth(label, labelSize) + 6.0f;
			const float termBudget = (rect.X + rect.W) - termX;
			const std::string shownTerm = termBudget <= 0.0f
				? std::string()
				: (ctx.MeasureTextWidth(term, termSize) <= termBudget
					? term
					: EllipsizeToWidth(ctx, term, termBudget, termSize));
			if (!shownTerm.empty())
				ctx.Commands().push_back({ WuiDrawKind::Text, { termX, rect.Y + (rect.H - termSize) * 0.5f, 0, 0 },
					theme.TextMuted, 0, 1.0f, shownTerm, termSize, false });
		}
		DrawFocusRing(ctx, rect, id, theme);
		return changed;
	}


bool CheckboxMixed(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label, bool& value, bool mixed, const WuiTheme& theme){
		bool changed = false;
		const bool focused = ctx.Focus() == id;
		const bool keyActivated = focused
			&& (ctx.WasKeyPressed(KeyCodes::Enter) || ctx.WasKeyPressed(KeyCodes::Space));
		if (ctx.IsClicked(rect) || keyActivated)
		{
			// 多选行的语义:混合态被点击 = "全部选中"(value 写回,调用方再分发给它管辖的对象)。
			mixed = false;
			value = true;
			changed = true;
		}
		// 无障碍节点:混合态 value="mixed",与 Checkbox 的 true/false 区分开(脚本据此判断三态)。
		RegisterAccessNode(id, "checkbox", rect, label,
			mixed ? "mixed" : (value ? "true" : "false"), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const WuiRect box { rect.X, rect.Y + (rect.H - 16.0f) * 0.5f, 16.0f, 16.0f };
		ctx.Commands().push_back({ WuiDrawKind::Rect, box, (value || mixed) ? theme.Accent : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, box, theme.Border, 3.0f, 1.0f });
		if (mixed)
		{
			// 混合:水平短横(—),而不是勾。
			ctx.Commands().push_back({ WuiDrawKind::Rect,
				{ box.X + 4.0f, box.Y + box.H * 0.5f - 1.0f, box.W - 8.0f, 2.0f }, theme.Text, 1.0f });
		}
		else if (value)
		{
			// 勾:两段斜线(Quad),不依赖字体里有没有 '✓' 字形。
			PushLineQuad(ctx, { box.X + 4.0f, box.Y + 8.5f }, { box.X + 7.0f, box.Y + 11.5f }, 2.0f, theme.Text);
			PushLineQuad(ctx, { box.X + 7.0f, box.Y + 11.5f }, { box.X + 12.0f, box.Y + 5.0f }, 2.0f, theme.Text);
		}
		ctx.Commands().push_back({ WuiDrawKind::Text, { rect.X + 24.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 },
			theme.Text, 0, 1.0f, label, 15.0f, false });
		DrawFocusRing(ctx, rect, id, theme);
		return changed;
	}


void SliderFloat(WuiContext& ctx, WuiId id, const WuiRect& rect, float& value, float min, float max, const WuiTheme& theme){
		// U2A 键盘微调步长:滑杆没有 drag 步长参数,按值域的 1%(0–1 滑杆 = 0.01/次)。
		constexpr float kKeyboardStepFraction = 0.01f;
		const bool focused = ctx.Focus() == id;
		RegisterAccessNode(id, "slider", rect, std::string(), FloatToText(value), true, true, focused);
		ctx.RegisterFocusable(id, rect);
		const float range = std::max(0.0001f, max - min);
		if (ctx.Input().MouseDown[0] && ctx.IsHovered(rect))
		{
			const float fraction = (ctx.Input().MousePos.x - rect.X) / std::max(1.0f, rect.W);
			value = min + std::max(0.0f, std::min(1.0f, fraction)) * range;
		}
		// U2A 键盘微调:焦点在滑杆上时左右箭头 ±1% 值域,并夹在 [min,max] 内。
		if (focused)
		{
			const float step = range * kKeyboardStepFraction;
			if (ctx.WasKeyPressed(KeyCodes::Left))
				value = std::max(min, value - step);
			if (ctx.WasKeyPressed(KeyCodes::Right))
				value = std::min(max, value + step);
		}

		const float trackH = 4.0f;
		const float trackY = rect.Y + rect.H * 0.5f - trackH * 0.5f;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, trackY, rect.W, trackH }, theme.ButtonBg, 2.0f });
		const float fraction = (value - min) / range;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, trackY, rect.W * fraction, trackH }, theme.Accent, 2.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X + rect.W * fraction - 4.0f, rect.Y + (rect.H - 12.0f) * 0.5f, 8.0f, 12.0f }, theme.Text, 2.0f });
		DrawFocusRing(ctx, rect, id, theme);
	}

namespace WuiWidgetsDetail
{

int Utf8Count(const std::string& text){
			int count = 0;
			for (size_t i = 0; i < text.size();)
			{
				const unsigned char c = static_cast<unsigned char>(text[i]);
				i += (c & 0x80) ? ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4)) : 1;
				++count;
			}
			return count;
		}


size_t Utf8Offset(const std::string& text, int cursor){
			if (cursor < 0) return text.size();
			size_t offset = 0;
			int index = 0;
			while (offset < text.size() && index < cursor)
			{
				const unsigned char c = static_cast<unsigned char>(text[offset]);
				offset += (c & 0x80) ? ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4)) : 1;
				++index;
			}
			return offset;
		}


		// 由像素位置估算光标字符下标:ASCII 半角按 0.52 倍字号,其余按全角宽度。
int CursorAtX(const std::string& text, float x, float fontSize){
			if (x <= 0)
				return 0;
			float accumulated = 0;
			int index = 0;
			for (size_t i = 0; i < text.size();)
			{
				const unsigned char c = static_cast<unsigned char>(text[i]);
				const int length = (c & 0x80) ? ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4)) : 1;
				const float width = (c < 0x80) ? fontSize * 0.52f : fontSize * 0.95f;
				if (x < accumulated + width * 0.5f)
					return index;
				accumulated += width;
				i += length;
				++index;
			}
			return index;
		}


void InsertUtf8At(std::string& text, size_t offset, uint32_t codepoint){
			std::string encoded;
			AppendUtf8(encoded, codepoint);
			text.insert(offset, encoded);
		}


void EraseBefore(std::string& text, int& cursor){
			const size_t offset = Utf8Offset(text, cursor);
			if (offset == 0) return;
			size_t start = offset - 1;
			while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
			text.erase(start, offset - start);
			--cursor;
		}


void EraseAt(std::string& text, int cursor){
			const size_t offset = Utf8Offset(text, cursor);
			if (offset >= text.size()) return;
			size_t end = offset + 1;
			while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
			text.erase(offset, end - offset);
		}


bool EditUpdate(WuiContext& ctx, std::string& buffer, int& cursor, int& selStart, int& selEnd, bool& submitted, bool& cancelled){
			submitted = false;
			cancelled = false;
			if (cursor < 0) cursor = Utf8Count(buffer);
			// P4-UX14:Ctrl+A 全选(用户实测"重命名时 Ctrl+A 失效")。放在插入之前,
			// 与后续的字符插入/光标移动互不干扰。
			if (ctx.Input().Ctrl && ctx.WasKeyPressed(KeyCodes::A))
			{
				selStart = 0;
				selEnd = Utf8Count(buffer);
				cursor = selEnd;
			}
			// MAT-UI8:剪贴板 Ctrl+C/X/V(2026-09-25 用户实测"Ctrl+F 查找框里只能手打" ——
			// 单行文本框此前只有 Ctrl+A,复制/剪切/粘贴全是 no-op)。索引口径:selStart/selEnd/
			// cursor 都是**码点**索引,与 buffer 的字节偏移之间走 Utf8Offset/Utf8Count。
			// 回调由宿主注入(见 WuiContext::GetClipboard/SetClipboard);未注入则静默 no-op。
			if (ctx.Input().Ctrl)
			{
				const bool hasSelection = selStart >= 0 && selEnd > selStart;
				if (ctx.WasKeyPressed(KeyCodes::C) && ctx.SetClipboard && hasSelection)
				{
					const size_t from = Utf8Offset(buffer, selStart);
					const size_t to = Utf8Offset(buffer, selEnd);
					ctx.SetClipboard(std::string_view(buffer).substr(from, to - from));
				}
				if (ctx.WasKeyPressed(KeyCodes::X) && ctx.SetClipboard && hasSelection)
				{
					const size_t from = Utf8Offset(buffer, selStart);
					const size_t to = Utf8Offset(buffer, selEnd);
					ctx.SetClipboard(std::string_view(buffer).substr(from, to - from));
					buffer.erase(from, to - from);
					cursor = selStart;
					selStart = -1;
					selEnd = -1;
				}
				if (ctx.WasKeyPressed(KeyCodes::V) && ctx.GetClipboard)
				{
					std::string clip;
					if (ctx.GetClipboard(clip) && !clip.empty())
					{
						// 单行控件:换行/回车直接丢掉(不插 '\n'、也不截成第一行),其余原样插入。
						clip.erase(std::remove(clip.begin(), clip.end(), '\r'), clip.end());
						clip.erase(std::remove(clip.begin(), clip.end(), '\n'), clip.end());
						const bool replacing = selStart >= 0 && selEnd > selStart;
						const int insertAt = replacing ? selStart : cursor;
						const size_t byteAt = Utf8Offset(buffer, insertAt);
						if (replacing)
							buffer.erase(byteAt, Utf8Offset(buffer, selEnd) - byteAt);
						buffer.insert(byteAt, clip);
						cursor = insertAt + Utf8Count(clip);
						selStart = -1;
						selEnd = -1;
					}
				}
			}
			for (uint32_t codepoint : ctx.Input().TextInput)
			{
				if (selStart >= 0 && selEnd > selStart)
				{
					buffer.erase(Utf8Offset(buffer, selStart), Utf8Offset(buffer, selEnd) - Utf8Offset(buffer, selStart));
					cursor = selStart;
				}
				selStart = -1;
				selEnd = -1;
				InsertUtf8At(buffer, Utf8Offset(buffer, cursor), codepoint);
				++cursor;
			}
			const int count = Utf8Count(buffer);
			if (cursor > count) cursor = count;
			// Shift 组合键扩展选区:靠近哪一端就移动哪一端,归零则取消选区。
			const bool shift = ctx.Input().Shift;
			// PROJ-17b(用户 2026-09-29:"那个位置的删除太灵敏了"):编辑键一律走**按下沿 + 系统重复**
			// (WasKeyTriggered = 本帧新按下 或 本帧 OS 重复事件),**不能**用 IsKeyPressed ——
			// 那是"按住就每帧为真"(KeyDown 列表),真人敲一次键横跨好几帧(60fps 下 80ms ≈ 5 帧),
			// 于是 Backspace/Delete 一次轻按会按帧数连删好几个字符(实测按住 6 帧 = 删 7 个字符),
			// 方向键/Home/End 也会一帧一格乱窜。按住连删的正确来源是 OS 重复事件(与系统"重复延迟/
			// 重复速度"一致),不是帧率。
			auto moveEdge = [&](int candidate)
			{
				if (selStart < 0 || selEnd <= selStart)
				{
					selStart = cursor;
					selEnd = cursor;
				}
				if (std::abs(candidate - selStart) <= std::abs(candidate - selEnd))
					selStart = candidate;
				else
					selEnd = candidate;
				cursor = candidate;
				if (selStart > selEnd) std::swap(selStart, selEnd);
				if (selStart == selEnd) { selStart = -1; selEnd = -1; }
			};
			if (ctx.WasKeyTriggered(KeyCodes::Left) && cursor > 0)
			{
				if (shift) moveEdge(cursor - 1);
				else { --cursor; selStart = -1; selEnd = -1; }
			}
			if (ctx.WasKeyTriggered(KeyCodes::Right) && cursor < count)
			{
				if (shift) moveEdge(cursor + 1);
				else { ++cursor; selStart = -1; selEnd = -1; }
			}
			if (ctx.WasKeyTriggered(KeyCodes::Home))
			{
				if (shift) moveEdge(0);
				else { cursor = 0; selStart = -1; selEnd = -1; }
			}
			if (ctx.WasKeyTriggered(KeyCodes::End))
			{
				if (shift) moveEdge(count);
				else { cursor = count; selStart = -1; selEnd = -1; }
			}
			if (ctx.WasKeyTriggered(KeyCodes::Backspace))
			{
				if (selStart >= 0 && selEnd > selStart)
				{
					buffer.erase(Utf8Offset(buffer, selStart), Utf8Offset(buffer, selEnd) - Utf8Offset(buffer, selStart));
					cursor = selStart;
					selStart = -1;
					selEnd = -1;
				}
				else if (cursor > 0)
					EraseBefore(buffer, cursor);
			}
			if (ctx.WasKeyTriggered(KeyCodes::Delete))
			{
				if (selStart >= 0 && selEnd > selStart)
				{
					buffer.erase(Utf8Offset(buffer, selStart), Utf8Offset(buffer, selEnd) - Utf8Offset(buffer, selStart));
					cursor = selStart;
					selStart = -1;
					selEnd = -1;
				}
				else if (cursor < Utf8Count(buffer))
					EraseAt(buffer, cursor);
			}
			if (ctx.WasKeyTriggered(KeyCodes::Enter)) submitted = true;
			if (ctx.WasKeyTriggered(KeyCodes::Escape)) cancelled = true;
			return submitted || cancelled;
		}


void PushTextFieldCommand(WuiContext& ctx, const WuiRect& rect, const std::string& text, const WuiTheme& theme, bool focused, bool hovered, int selStart, int selEnd){
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonBg, 3.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, focused ? theme.Accent : theme.Border, 3.0f, focused ? 1.5f : 1.0f });
			WuiDrawCommand command { WuiDrawKind::Text, { rect.X + 6.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 }, theme.Text, 0, 1.0f, text, 15.0f, false };
			command.TextSelStart = (focused && selStart >= 0 && selEnd > selStart) ? static_cast<int>(Utf8Offset(text, selStart)) : -1;
			command.TextSelEnd = (focused && selStart >= 0 && selEnd > selStart) ? static_cast<int>(Utf8Offset(text, selEnd)) : -1;
			ctx.Commands().push_back(std::move(command));
			(void)hovered;
		}


		// ---- U24 数值控件的公共件 ----

		// 显示用浮点文本:定点输出后去尾零(0.300 → "0.3",整数 → "0")。
		// 注意:既有 kind="slider"/"drag-float" 的无障碍 value 仍用 FloatToText("%.3f"),
		// 只有新增控件用这里的"有效位"文本,避免改既有节点的 value 语义。
std::string TrimNumberText(std::string text){
			if (text.find('.') == std::string::npos)
				return text;
			size_t last = text.find_last_not_of('0');
			if (last == std::string::npos)
				return "0";
			if (text[last] == '.')
				--last;
			text.erase(last + 1);
			if (text == "-0")
				text = "0";
			return text;
		}

}
}
