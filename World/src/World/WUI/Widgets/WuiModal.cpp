#include "wldpch.h"
#include "World/WUI/Widgets/WuiModal.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiAccessibility.h"

#include <algorithm>

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

	ModalResult ModalFooter(WuiContext& ctx, const WuiRect& frameRect, const std::string& confirmLabel,
		const std::string& cancelLabel, WuiId confirmId, WuiId cancelId, bool confirmEnabled,
		const WuiTheme& theme)
	{
		constexpr float fontSize = 15.0f;
		constexpr float gap = 8.0f;
		const float y = frameRect.Y + frameRect.H - ModalFooterPadding - ModalFooterHeight;
		// 按文字宽度定按钮宽(有下限),窄框时整体压缩,保证两个按钮都留在框内。
		float confirmW = std::min(200.0f, std::max(110.0f, ctx.MeasureTextWidth(confirmLabel, fontSize) + 30.0f));
		float cancelW = std::min(160.0f, std::max(90.0f, ctx.MeasureTextWidth(cancelLabel, fontSize) + 30.0f));
		const float available = std::max(80.0f, frameRect.W - ModalFooterPadding * 2.0f);
		if (confirmW + cancelW + gap > available)
		{
			const float scale = std::max(0.35f, (available - gap) / (confirmW + cancelW));
			confirmW = std::max(48.0f, confirmW * scale);
			cancelW = std::max(40.0f, cancelW * scale);
		}
		const WuiRect confirmRect { frameRect.X + ModalFooterPadding, y, confirmW, ModalFooterHeight };
		const WuiRect cancelRect { confirmRect.X + confirmRect.W + gap, y, cancelW, ModalFooterHeight };

		bool confirmClicked = false;
		if (confirmEnabled)
			confirmClicked = Button(ctx, confirmId, confirmRect, confirmLabel, theme);
		else
		{
			// 禁用态:弱化绘制 + 登记 enabled=false / interactive=false(ui.invoke 不会点到它)。
			RegisterAccessNode(confirmId, "button", confirmRect, confirmLabel, std::string(), false, false);
			ctx.Commands().push_back({ WuiDrawKind::Rect, confirmRect, theme.ButtonBg, 3.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, confirmRect, theme.Border, 3.0f, 1.0f });
			ctx.Commands().push_back({ WuiDrawKind::Text,
				{ confirmRect.X + 8.0f, confirmRect.Y + (confirmRect.H - fontSize) * 0.5f, 0, 0 },
				theme.TextMuted, 0.0f, 1.0f, confirmLabel, fontSize, false });
		}
		// 两个按钮每帧都绘制/登记:确认被点时不能让取消按钮消失一帧。
		const bool cancelClicked = Button(ctx, cancelId, cancelRect, cancelLabel, theme);

		if (confirmClicked)
			return ModalResult::Confirm;
		if (cancelClicked)
			return ModalResult::Cancel;
		return ModalResult::None;
	}
}
