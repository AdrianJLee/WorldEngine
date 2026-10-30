#include "wldpch.h"
#include "WUI/Panels/WindowsPanel.h"
#include "WUI/Panels/PhysicsPainterPanel.h"

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
		{
			Wui::Label(ctx, { rect.X + 8, y + 4 }, "拖动面板标签页到窗口外即可创建独立窗口。", theme.TextMuted, 13.0f);
			y += 24.0f;
		}

		y += 12.0f;
		// 关卡创作与视口工具快捷入口 (Level Authoring & Viewport Tools)
		const float startX = rect.X + 8.0f;
		const float contentW = std::max(120.0f, rect.W - 16.0f);
		Wui::Label(ctx, { startX, y }, "关卡创作与视口工具 (Level Authoring Tools)", theme.Accent, theme.FontSizeSmall);
		y += 20.0f;

		const Wui::WuiRect toolCardRect { startX, y, contentW, 58.0f };
		Wui::PanelBackground(ctx, toolCardRect, theme.ContentBg, 4.0f);
		Wui::HighlightOutline(ctx, toolCardRect, theme.Border, 4.0f, 1.0f);

		const bool painterActive = PhysicsPainterPanel::IsActive();
		const std::string painterStatus = painterActive ? "[已激活 ON]" : "[已关闭 OFF]";
		Wui::Label(ctx, { toolCardRect.X + 8.0f, toolCardRect.Y + 8.0f },
			"物理画笔 (Physics Painter): " + painterStatus,
			painterActive ? theme.Success : theme.Text, theme.FontSizeSmall);

		char pDetail[128];
		std::snprintf(pDetail, sizeof(pDetail), "半径: %.1fm | 密度: %d 个/次",
			PhysicsPainterPanel::GetBrushRadius(), PhysicsPainterPanel::GetBrushDensity());
		Wui::Label(ctx, { toolCardRect.X + 8.0f, toolCardRect.Y + 28.0f }, pDetail, theme.TextMuted, 11.0f);

		const float toggleBtnW = 90.0f;
		const Wui::WuiRect toggleBtn { toolCardRect.X + toolCardRect.W - toggleBtnW - 8.0f, toolCardRect.Y + 8.0f, toggleBtnW, 22.0f };
		if (Wui::Button(ctx, Wui::HashId("windows.toggle.physicspainter"), toggleBtn,
			painterActive ? "关闭画笔" : "开启画笔", theme))
		{
			PhysicsPainterPanel::ToggleActive();
		}
		Wui::Tooltip(ctx, toggleBtn, "切换 3D 视口贴地物理射线散布画笔开关");

		const float openBtnW = 90.0f;
		const Wui::WuiRect openBtn { toolCardRect.X + toolCardRect.W - openBtnW - 8.0f, toolCardRect.Y + 32.0f, openBtnW, 20.0f };
		if (Wui::Button(ctx, Wui::HashId("windows.open.physicspainter"), openBtn, "聚焦面板", theme))
		{
			host.FocusIndependentWindow("physicspainter");
		}
		Wui::Tooltip(ctx, openBtn, "聚焦或打开独立的物理画笔配置面板");
	}
}
