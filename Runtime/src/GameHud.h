#pragma once

#include "World/WUI/WuiContext.h"

#include <cstddef>

namespace World
{
	// 游戏 HUD 示例:证明游戏 UI 与编辑器共用同一套 WUI 控件库。
	void DrawGameHud(Wui::WuiContext& ctx, size_t entityCount);
}
