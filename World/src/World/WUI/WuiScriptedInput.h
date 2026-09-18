#pragma once

#include "World/Core/Export.h"
#include "World/WUI/WuiContext.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace World::Wui
{
	// 脚本化输入:AI 控制通道用**真实输入注入**代替"直接调用业务函数"。
	// 每个窗口各有一份待注入的指针/文本事件;窗口在自己的帧开始处调用 Apply(),
	// 之后控件看到的 hover/click/字符输入与真实操作完全同一条路径(不是测试专用分支)。
	//
	// 注入节拍:第 1 帧 press(位置 + MouseDown + MouseClicked),第 2 帧 release;
	// 之后(若有文本)按行逐帧写字符:每帧一段文本,第 2 段起先注入一次 Enter 键,
	// 与"点击聚焦 → 键入"的真实帧序列一致,滑条/下拉/菜单/文本控件都正常响应。
	class WLD_API WuiScriptedInput
	{
	public:
		static WuiScriptedInput& Get();

		// 在指定窗口的指定客户区坐标点一次(1 次点击 = press → release)。
		void QueueClick(const std::string& windowKey, glm::vec2 position);
		// 在指定窗口注入一段文本(UTF-8)。语义:点击(QueueClick)完成后**下一帧**开始注入,
		// 按 '\n' 分行、每帧一段(第 2 段起该帧先注入 Enter 键再写该行码点),'\r' 并入换行;
		// 中文等非 ASCII 按 UTF-8 解码成码点写入 input.TextInput —— 与真实键盘上屏同一条路径。
		// 同一窗口同时只有一份待注入输入;ui.type = QueueClick(聚焦) + QueueType。
		void QueueType(const std::string& windowKey, std::string text);
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
			// 文本阶段(点击完成后开始):按行拆分的码点,每帧消费一段。
			std::vector<std::vector<uint32_t>> TextFrames;
			size_t NextTextFrame = 0;
		};
		std::unordered_map<std::string, Pending> m_Pending;
	};
}
