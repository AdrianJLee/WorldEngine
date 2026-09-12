#include "wldpch.h"
#include "World/WUI/WuiDemo.h"
#include "World/WUI/WuiWidgets.h"

namespace World::Wui
{
	void ShowDemoPanel(WuiContext& ctx, const WuiRect& rect)
	{
		const WuiTheme theme;
		Panel(ctx, rect, "WUI Demo", theme);

		int& counter = ctx.Persist<int>(HashId("demo.counter"), 0);
		float& slider = ctx.Persist<float>(HashId("demo.slider"), 0.5f);

		float y = rect.Y + 36.0f;
		Label(ctx, { rect.X + 12.0f, y }, "WUI 核心链路演示(中文/布局/交互)", theme.TextMuted, 14.0f);
		y += 22.0f;

		if (Button(ctx, HashId("demo.inc"), { rect.X + 12.0f, y, 150.0f, 26.0f }, "Count +1", theme))
			++counter;
		Label(ctx, { rect.X + 176.0f, y + 5.0f }, "count = " + std::to_string(counter), theme.Text, 15.0f);
		y += 34.0f;

		const bool toggle = Toggle(ctx, HashId("demo.toggle"), { rect.X + 12.0f, y, 220.0f, 20.0f }, "Enabled (persist state)", theme);
		y += 28.0f;

		SliderFloat(ctx, HashId("demo.slider"), { rect.X + 12.0f, y, 220.0f, 18.0f }, slider, 0.0f, 1.0f, theme);
		y += 24.0f;

		Label(ctx, { rect.X + 12.0f, y }, toggle ? "toggle: on, slider = " + std::to_string(slider) : "toggle: off", theme.Accent, 14.0f);
		y += 24.0f;

		Label(ctx, { rect.X + 12.0f, y }, "你好, WorldEngine / 中文渲染验证", theme.Text, 15.0f);
	}
}
