#pragma once

#include "World/Core/Export.h"
#include "World/WUI/WuiContext.h"

#include <string>
#include <unordered_map>

namespace World::Wui
{
	// 脚本化输入:AI 控制通道用**真实输入注入**代替"直接调用业务函数"。
	// 每个窗口各有一份待注入的指针事件;窗口在自己的帧开始处调用 Apply(),
	// 之后控件看到的 hover/click 与鼠标操作完全同一条路径(不是测试专用分支)。
	//
	// 注入节拍:第 1 帧 press(位置 + MouseDown + MouseClicked),第 2 帧 release,
	// 第 3 帧清空 —— 与真实点击的帧序列一致,滑条/下拉/菜单都能正常响应。
	class WLD_API WuiScriptedInput
	{
	public:
		static WuiScriptedInput& Get();

		// 在指定窗口的指定客户区坐标点一次(1 次点击 = press → release)。
		void QueueClick(const std::string& windowKey, glm::vec2 position);
		// 是否还有待注入事件(供自动化等待"注入被消费")。
		bool HasPending() const;
		// 每个窗口每帧调用一次:把待注入事件写进该窗口的输入状态。
		void Apply(const std::string& windowKey, WuiInputState& input);

	private:
		struct Pending
		{
			glm::vec2 Position { 0, 0 };
			int FramesLeft = 0;
			int Phase = 0;   // 0 = press, 1 = release
		};
		std::unordered_map<std::string, Pending> m_Pending;
	};
}
