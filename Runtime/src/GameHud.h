#pragma once

#include "World/WUI/WuiContext.h"

#include <cstddef>
#include <string>

namespace World
{
	// 游戏 HUD 示例:证明游戏 UI 与编辑器共用同一套 WUI 控件库。
	void DrawGameHud(Wui::WuiContext& ctx, size_t entityCount);

	// T5c 流量加载界面:资产还在后台解析时覆盖全屏(名称 + 进度),解析完由宿主停止调用。
	// 与 HUD 同一套 WUI 控件 —— 不需要第二份 UI 实现。
	void DrawLoadingOverlay(Wui::WuiContext& ctx, std::size_t pending, std::size_t failed,
		const std::string& summary);
}
