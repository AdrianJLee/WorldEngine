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

	// T5c:流式加载界面。整屏压暗 + 居中卡片;进度是**真实数字**(不是假动画)——
	// 后台解析的 pending 数由 AsyncLoader 提供,自动化可以直接断言文字内容。
	void DrawLoadingOverlay(Wui::WuiContext& ctx, std::size_t pending, std::size_t failed,
		const std::string& summary)
	{
		const Wui::WuiTheme theme;
		const glm::vec2 size = ctx.Input().ViewportSize;
		// 压暗整屏(让下面的场景/清屏色退到背景)。
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { 0.0f, 0.0f, size.x, size.y },
			{ 0.0f, 0.0f, 0.0f, 0.62f }, 0.0f, 1.0f, "", 0.0f, false, 0, { 0, 0, 1, 1 }, -1, -1, -1 });

		const float width = 420.0f;
		const float height = 116.0f;
		const Wui::WuiRect card {
			(size.x - width) * 0.5f, (size.y - height) * 0.5f, width, height };
		Panel(ctx, card, "Loading", theme);
		Label(ctx, { card.X + 16.0f, card.Y + 38.0f },
			"Streaming assets from disk (background deserialization)", theme.Text, 14.0f);
		Label(ctx, { card.X + 16.0f, card.Y + 62.0f },
			"pending: " + std::to_string(pending) + (failed > 0 ? ("   failed: " + std::to_string(failed)) : std::string()),
			theme.Text, 14.0f);
		Label(ctx, { card.X + 16.0f, card.Y + 86.0f }, summary, theme.TextMuted, 12.0f);
	}
}
