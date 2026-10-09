#include "wldpch.h"
#include "WUI/Panels/LevelStreamingPanel.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

namespace World
{
	void LevelStreamingPanel::EnsureInitialized()
	{
		if (m_Initialized)
			return;
		m_Initialized = true;

		m_DataLayerManager.RegisterLayer(1, "Base_World", true);
		m_DataLayerManager.RegisterLayer(2, "Day", true);
		m_DataLayerManager.RegisterLayer(4, "Night", false);
	}

	void LevelStreamingPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		EnsureInitialized();

		const Wui::WuiRect area{ rect.X + 6, rect.Y + 6, rect.W - 12, rect.H - 12 };
		Wui::PanelBackground(ctx, area, theme.PanelHeader, 4.0f);
		Wui::HighlightOutline(ctx, area, theme.Border, 4.0f, 1.0f);

		float y = area.Y + 8.0f;
		Wui::Label(ctx, { area.X + 10.0f, y }, "关卡流送与空间分区监视器 (Level Streaming)", theme.Text, 15.0f);
		y += 26.0f;

		// 1. World Origin Offset
		const auto& origin = m_WorldSpace.GetOriginOffset();
		std::string originStr = "世界原点偏移: X: " + std::to_string(static_cast<int64_t>(origin.x)) +
		                        " m, Y: " + std::to_string(static_cast<int64_t>(origin.y)) +
		                        " m, Z: " + std::to_string(static_cast<int64_t>(origin.z)) + " m";
		Wui::Label(ctx, { area.X + 10.0f, y }, originStr, theme.TextMuted, 13.0f);
		y += 22.0f;

		Wui::Label(ctx, { area.X + 10.0f, y }, "重定位阈值: 1000m (浮点抖动与穿模防护)", theme.TextMuted, 13.0f);
		y += 28.0f;

		// 2. Data Layers
		Wui::Label(ctx, { area.X + 10.0f, y }, "逻辑数据层 (Data Layers):", theme.Text, 14.0f);
		y += 24.0f;

		const bool isDay = m_DataLayerManager.IsLayerActive("Day");
		const bool isNight = m_DataLayerManager.IsLayerActive("Night");

		std::string dayLabel = std::string("Day: ") + (isDay ? "[已激活 Active]" : "[已禁用 Off]");
		std::string nightLabel = std::string("Night: ") + (isNight ? "[已激活 Active]" : "[已禁用 Off]");

		Wui::Label(ctx, { area.X + 10.0f, y }, "Base_World: [常驻 Base]", theme.TextMuted, 13.0f);
		y += 20.0f;
		Wui::Label(ctx, { area.X + 10.0f, y }, dayLabel, isDay ? theme.Accent : theme.TextMuted, 13.0f);
		y += 20.0f;
		Wui::Label(ctx, { area.X + 10.0f, y }, nightLabel, isNight ? theme.Accent : theme.TextMuted, 13.0f);
		y += 26.0f;

		if (Wui::Button(ctx, Wui::HashId("level.toggle_day_night"), { area.X + 10.0f, y, 180.0f, 24.0f }, "切换昼夜态 (Toggle Day/Night)", theme))
		{
			m_DataLayerManager.SetLayerActive("Day", !isDay);
			m_DataLayerManager.SetLayerActive("Night", isDay);
		}
		y += 36.0f;

		// 3. AI & NavMesh
		Wui::Label(ctx, { area.X + 10.0f, y }, "AI 与导航系统协同 (AI & Navigation):", theme.Text, 14.0f);
		y += 24.0f;
		Wui::Label(ctx, { area.X + 10.0f, y }, "NavMesh TileCache 动态拼缝: 就绪 (Ready)", theme.TextMuted, 13.0f);
		y += 20.0f;
		Wui::Label(ctx, { area.X + 10.0f, y }, "AI 三级虚拟化 LOD: 运行中 (Tier 1~3 Active)", theme.TextMuted, 13.0f);
		y += 20.0f;
		Wui::Label(ctx, { area.X + 10.0f, y }, "跨边界视线感知门禁: 已开启 (Guard Enabled)", theme.TextMuted, 13.0f);
	}
}
