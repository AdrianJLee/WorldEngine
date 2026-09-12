#pragma once

#include "World/WUI/WuiContext.h"

namespace World::Wui
{
	// 渲染/输入后端抽象。Core 只产出绘制命令与读取输入,后端负责呈现。
	class WuiBackend
	{
	public:
		virtual ~WuiBackend() = default;
		virtual bool BeginFrame(WuiInputState& input) = 0;
		virtual void Render(const std::vector<WuiDrawCommand>& commands, const std::vector<WuiDrawCommand>& overlayCommands) = 0;
		virtual void EndFrame(WuiCursor cursor = WuiCursor::Arrow) = 0;
	};
}
