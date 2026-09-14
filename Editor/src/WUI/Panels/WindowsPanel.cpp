#include "wldpch.h"
#include "WindowsPanel.h"

#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

namespace World
{
	void WindowsPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		const size_t count = host.IndependentWindowCount();
		Wui::Label(ctx, { rect.X + 8, rect.Y + 6 }, "独立窗口(脱离停靠):" + std::to_string(count), theme.TextMuted, 13.0f);

		float y = rect.Y + 26.0f;
		for (size_t i = 0; i < count; ++i)
		{
			const std::string panel = host.IndependentWindowPanel(i);
			const std::string label = host.IndependentWindowLabel(i);
			const Wui::WuiRect row { rect.X + 6, y, std::max(120.0f, rect.W - 12), 26.0f };
			Wui::HoverRow(ctx, row, ctx.IsHovered(row), false, theme);
			Wui::Label(ctx, { row.X + 8, row.Y + 6 }, label, theme.Text, 14.0f);

			const Wui::WuiRect focus { row.X + row.W - 150, row.Y + 2, 62, 22 };
			const Wui::WuiRect dockBack { row.X + row.W - 82, row.Y + 2, 74, 22 };
			if (Wui::Button(ctx, Wui::HashId(("window.focus." + panel).c_str()), focus, "Focus", theme))
				host.FocusIndependentWindow(panel);
			if (Wui::Button(ctx, Wui::HashId(("window.dock." + panel).c_str()), dockBack, "Dock", theme))
				host.DockBackIndependentWindow(panel);
			y += 30.0f;
		}

		if (count == 0)
			Wui::Label(ctx, { rect.X + 8, y + 4 }, "拖动面板标签页到窗口外即可创建独立窗口。", theme.TextMuted, 13.0f);
	}
}
