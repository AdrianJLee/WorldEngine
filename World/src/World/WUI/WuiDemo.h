#pragma once

#include "World/WUI/WuiContext.h"

namespace World::Wui
{
	// W1 演示面板:验证 Core + 控件 + Backend 全链路;W2 编辑器迁移后移除。
	void ShowDemoPanel(WuiContext& ctx, const WuiRect& rect);
}
