#include "wldpch.h"
#include "World/WUI/Widgets/WuiModal.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"

#include <algorithm>
#include <vector>

namespace World::Wui
{
	namespace
	{
		// 与 WuiWidgets.cpp 的登记口径一致:窗口/面板取当前上下文,矩形是窗口客户区坐标。
		void RegisterAccessNode(WuiId id, const std::string& kind, const WuiRect& rect,
			const std::string& label, const std::string& value, bool enabled, bool interactive)
		{
			if (id == 0)
				return;
			WuiAccessNode node;
			node.Id = id;
			node.Window = WuiAccessibility::Get().CurrentWindow();
			node.Panel = WuiAccessibility::Get().CurrentPanel();
			node.Kind = kind;
			node.Label = label;
			node.Value = value;
			node.Rect = rect;
			node.Enabled = enabled;
			node.Interactive = interactive;
			WuiAccessibility::Get().Register(node);
		}
	}

	void BeginModalInputBlock(WuiContext& ctx)
	{
		const glm::vec2 viewport = ctx.ViewportSize();
		ctx.PushHoverBlocker({ 0.0f, 0.0f, viewport.x, viewport.y });
	}

	void EndModalInputBlock(WuiContext& ctx)
	{
		// 此时模态要画的控件都已到绘制阶段,清空遮挡区即可让它们正常命中
		// (与迁移前宿主在 DrawModals 之前 ClearHoverBlockers 的效果一致)。
		ctx.ClearHoverBlockers();
	}

	bool BeginModalFrame(WuiContext& ctx, const ModalFrameDesc& desc, WuiRect* frameRect,
		bool* escapePressed, const WuiTheme& theme)
	{
		if (desc.Id == 0 || ctx.Modal() != desc.Id)
			return false;
		ctx.PushOverlay();

		const glm::vec2 viewport = ctx.ViewportSize();
		const WuiRect frame { (viewport.x - desc.Size.x) * 0.5f, (viewport.y - desc.Size.y) * 0.5f,
			desc.Size.x, desc.Size.y };
		if (frameRect)
			*frameRect = frame;

		// 遮罩 + 面板:与 Wui::BeginModal 相同的配色/圆角/图层(overlay 通道)。
		ctx.Commands().push_back({ WuiDrawKind::Rect, { 0, 0, viewport.x, viewport.y }, { 0, 0, 0, 0.5f }, 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, frame, theme.PanelBg, 5.0f });

		// 标题栏:统一高度 + 垂直居中标题(BeginModal 时代是裸文字,这里补上标题条)。
		// 标题条先画、外框描边后画,保证边框完整压在标题条之上。
		const float titleHeight = std::max(20.0f, desc.TitleHeight);
		const WuiRect titleBar { frame.X, frame.Y, frame.W, titleHeight };
		constexpr float titleFont = 16.0f;
		ctx.Commands().push_back({ WuiDrawKind::Rect, titleBar, theme.PanelHeader, 5.0f });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, frame, theme.Border, 5.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Text,
			{ frame.X + 14.0f, frame.Y + std::max(2.0f, (titleHeight - titleFont) * 0.5f), 0, 0 },
			theme.Text, 0.0f, 1.0f, desc.Title, titleFont, true });
		// 无障碍:标题节点 id = 模态自身的 WuiId(即 HashId("modal.<名字>")),kind=text 只读。
		RegisterAccessNode(desc.Id, "text", titleBar, desc.Title, desc.Title, true, false);

		if (escapePressed && ctx.IsKeyPressed(KeyCodes::Escape))
			*escapePressed = true;
		return true;
	}

	void EndModalFrame(WuiContext& ctx)
	{
		ctx.PopOverlay();
	}

	int ModalButtons(WuiContext& ctx, const WuiRect& frameRect, const ModalButtonDesc* buttons,
		std::size_t count, const WuiTheme& theme)
	{
		constexpr float gap = 8.0f;
		constexpr float minFlexWidth = 48.0f;
		// 单按钮/两按钮在宽框里不拉满整框(与 ModalFooter 的按钮宽度上限一致)。
		constexpr float maxFlexWidth = 240.0f;
		if (buttons == nullptr || count == 0)
			return -1;

		const float available = std::max(80.0f, frameRect.W - ModalFooterPadding * 2.0f);
		const float totalGap = gap * static_cast<float>(count - 1);
		std::vector<float> widths(count, 0.0f);
		float fixedTotal = 0.0f;
		std::size_t flexCount = 0;
		for (std::size_t i = 0; i < count; ++i)
		{
			if (buttons[i].Width > 0.0f)
			{
				widths[i] = buttons[i].Width;
				fixedTotal += buttons[i].Width;
			}
			else
				++flexCount;
		}
		if (flexCount > 0)
		{
			const float share = (available - totalGap - fixedTotal) / static_cast<float>(flexCount);
			const float flexWidth = std::max(minFlexWidth, std::min(maxFlexWidth, share));
			for (std::size_t i = 0; i < count; ++i)
				if (widths[i] <= 0.0f)
					widths[i] = flexWidth;
		}
		float rowWidth = totalGap;
		for (const float width : widths)
			rowWidth += width;
		// 极窄框(按钮总宽超过可用宽度)时整体等比压缩,保证按钮都留在框内。
		if (rowWidth > available)
		{
			const float scale = std::max(0.2f, (available - totalGap) / std::max(1.0f, rowWidth - totalGap));
			rowWidth = totalGap;
			for (float& width : widths)
			{
				width = std::max(24.0f, width * scale);
				rowWidth += width;
			}
		}
		// 全是弹性按钮时整行居中(等分到满宽时居中 == 贴左内边距);带固定宽度(ModalFooter)
		// 时保持原来的左对齐口径,已有模态的按钮位置不变。
		float x = fixedTotal > 0.0f ? frameRect.X + ModalFooterPadding
			: frameRect.X + (frameRect.W - rowWidth) * 0.5f;
		const float y = frameRect.Y + frameRect.H - ModalFooterPadding - ModalFooterHeight;

		int clicked = -1;
		for (std::size_t i = 0; i < count; ++i)
		{
			const WuiRect rect { x, y, widths[i], ModalFooterHeight };
			if (buttons[i].Enabled)
			{
				if (Button(ctx, buttons[i].Id, rect, buttons[i].Label, theme) && clicked < 0)
					clicked = static_cast<int>(i);
			}
			else
			{
				// 禁用态:弱化绘制 + 登记 enabled=false / interactive=false(ui.invoke 不会点到它)。
				RegisterAccessNode(buttons[i].Id, "button", rect, buttons[i].Label, std::string(), false, false);
				ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonBg, 3.0f });
				ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, theme.Border, 3.0f, 1.0f });
				ctx.Commands().push_back({ WuiDrawKind::Text,
					{ rect.X + 8.0f, rect.Y + (rect.H - 15.0f) * 0.5f, 0, 0 },
					theme.TextMuted, 0.0f, 1.0f, buttons[i].Label, 15.0f, false });
			}
			x += widths[i] + gap;
		}
		// 所有按钮每帧都绘制/登记:某个按钮被点时不能让其它按钮消失一帧。
		return clicked;
	}

	ModalResult ModalFooter(WuiContext& ctx, const WuiRect& frameRect, const std::string& confirmLabel,
		const std::string& cancelLabel, WuiId confirmId, WuiId cancelId, bool confirmEnabled,
		const WuiTheme& theme)
	{
		constexpr float fontSize = 15.0f;
		// 原有宽度口径(按文字宽度定,有下限/上限)原样保留,交给通用按钮条的固定宽度字段。
		const float confirmW = std::min(200.0f, std::max(110.0f, ctx.MeasureTextWidth(confirmLabel, fontSize) + 30.0f));
		const float cancelW = std::min(160.0f, std::max(90.0f, ctx.MeasureTextWidth(cancelLabel, fontSize) + 30.0f));
		const ModalButtonDesc buttons[2] = {
			{ confirmLabel, confirmId, confirmEnabled, confirmW },
			{ cancelLabel, cancelId, true, cancelW },
		};
		const int clicked = ModalButtons(ctx, frameRect, buttons, 2, theme);
		if (clicked == 0)
			return ModalResult::Confirm;
		if (clicked == 1)
			return ModalResult::Cancel;
		return ModalResult::None;
	}
}
