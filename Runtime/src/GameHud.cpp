#include "wldpch.h"
#include "GameHud.h"
#include "World/WUI/WuiWidgets.h"

namespace World
{
	void DrawGameHud(Wui::WuiContext& ctx, size_t entityCount)
	{
		const Wui::WuiTheme theme;
		const Wui::WuiRect panel { 12, 12, 260, 132 };
		Panel(ctx, panel, "Game HUD", theme);
		Label(ctx, { panel.X + 12, panel.Y + 36 }, "Entities: " + std::to_string(entityCount), theme.Text, 14.0f);
		Label(ctx, { panel.X + 12, panel.Y + 58 }, "FPS: " + std::to_string(static_cast<int>(ctx.Input().FPS)), theme.Text, 14.0f);
		Toggle(ctx, Wui::HashId("hud.debug"), { panel.X + 12, panel.Y + 80, 180, 20 }, "Debug overlay", theme);
		Label(ctx, { panel.X + 12, panel.Y + 104 }, "与编辑器同一套 WUI", theme.TextMuted, 13.0f);
	}
}
