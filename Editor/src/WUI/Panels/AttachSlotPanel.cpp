#include "wldpch.h"
#include "AttachSlotPanel.h"

#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

namespace World
{
	void AttachSlotPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const bool highlighted = host.AttachSlotHighlighted();
		const Wui::WuiColor fill = highlighted ? Wui::WuiColor { 0.3f, 0.5f, 0.9f, 0.30f } : theme.PanelHeader;
		const Wui::WuiColor border = highlighted ? Wui::WuiColor { 0.45f, 0.65f, 1.0f, 1.0f } : theme.Border;

		const Wui::WuiRect area { rect.X + 6, rect.Y + 6, rect.W - 12, rect.H - 12 };
		Wui::PanelBackground(ctx, area, fill, 4.0f);
		Wui::HighlightOutline(ctx, area, border, 4.0f, 2.0f);
		Wui::Label(ctx, { area.X + 10, area.Y + 8 }, "挂靠槽位(Attach Slot)", theme.Text, 14.0f);
		Wui::Label(ctx, { area.X + 10, area.Y + 28 }, "把独立窗口拖到此处松手即可挂靠回主窗口。", theme.TextMuted, 13.0f);

		// 兜底入口:不拖动也能逐窗挂靠。
		float y = area.Y + 52.0f;
		const size_t count = host.IndependentWindowCount();
		for (size_t i = 0; i < count; ++i)
		{
			const std::string panel = host.IndependentWindowPanel(i);
			if (Wui::Button(ctx, Wui::HashId(("attach." + panel).c_str()), { area.X + 10, y, 150, 22 }, "Attach " + panel, theme))
				host.AttachIndependentWindowToSlot(panel);
			y += 26.0f;
		}
		if (count == 0)
			Wui::Label(ctx, { area.X + 10, y }, "(当前没有独立窗口)", theme.TextMuted, 13.0f);
	}
}
