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
		// button 与 WuiInputState 的 MouseDown/Clicked/Released 下标一致:0 = 左键(默认,
		// 保持既有调用语义),1 = 右键,2 = 中键。控件侧读到的仍是普通输入 —— 右键用来
		// 复现控件自己的上下文菜单路径(TreeView/ListView/GridView 的 ContextClicked)。
		// ctrl/shift = 这一次点击期间**按住**的修饰键:面板的"Ctrl+点击 = 加选(多选)"、
		// "Shift+点击 = 范围/等比"这类路径只能靠它驱动(AI 无障碍操作多选的前提;
		// 走的是与真人按键同一条 `WuiInputState.Ctrl/Shift` 口径,不是测试专用分支)。
		void QueueClick(const std::string& windowKey, glm::vec2 position, int button = 0,
			bool ctrl = false, bool shift = false);
		// 在指定窗口注入一段文本(UTF-8)。语义:点击(QueueClick)完成后**下一帧**开始注入,
		// 按 '\n' 分行、每帧一段(第 2 段起该帧先注入 Enter 键再写该行码点),'\r' 并入换行;
		// 中文等非 ASCII 按 UTF-8 解码成码点写入 input.TextInput —— 与真实键盘上屏同一条路径。
		// 同一窗口同时只有一份待注入输入;ui.type = QueueClick(聚焦) + QueueType。
		void QueueType(const std::string& windowKey, std::string text);
		// 注入一次按键(第 1 帧按下、第 2 帧释放):用于 AI 复现方向键/Enter 等键路。
		// holdFrames > 0 时,按下帧之后**继续按住** holdFrames 帧再释放 —— 真人敲一次键
		// 通常横跨好几帧(60fps 下 80ms ≈ 5 帧),只按 1 帧会漏掉"按住连触发"这类缺陷
		// (用户 2026-09-29:"那个位置的删除太灵敏了" = 单击一次删掉好几个字符)。
		// 按住期间只写 KeyDown、不写 KeyRepeated:与"未到系统重复延迟的短按"一致。
		// ctrl/shift = 注入这两帧的修饰键状态。真实键盘的 Ctrl+A 在 ctx.Input().Ctrl 上,
		// 而 keybd_event 合成的 Ctrl **到不了 WUI 的输入状态**(两次实测),所以组合键只能
		// 从这里注入 —— 否则"框内 Ctrl+A"这类作用域问题永远只能靠人眼观察。
		void QueueKey(const std::string& windowKey, uint32_t keyCode, bool ctrl = false, bool shift = false,
			int holdFrames = 0);
		// 注入一次滚轮(第 1 帧写 MousePos + Wheel,下一帧自动清掉)。滚动区/列表的
		// "滚不动"类问题只能靠滚轮复现 —— 键盘 ↑/↓ 与拖动滚动条是另外两条路径,不能互相证明。
		void QueueWheel(const std::string& windowKey, glm::vec2 position, float wheel);
		// 是否还有待注入事件(供自动化等待"注入被消费")。
		bool HasPending() const;
		// 宿主**每帧开头**调用一次(在游戏 UI 采样与 WUI 帧之前):开启本帧的注入快照窗口。
		// 同一帧内多个消费者(游戏 UI 的输入采样 + WUI 控件帧)各读一次同一份注入结果,相位
		// 每帧只推进一次 —— 真人点一下本来就同时落在两个系统上,拆成"press 给一边、release
		// 给另一边"是错的。未调用过 BeginFrame 的调用方(单元测试逐帧 Apply)保持旧语义。
		void BeginFrame();
		// 把本帧待注入的输入写进 `input`(可被同一帧的多个消费者各调一次)。
		void Apply(const std::string& windowKey, WuiInputState& input);

	private:
		// 本帧的注入结果(相位推进一次的结果 + "是否写了指针位置")。
		struct FrameInjection
		{
			WuiInputState State;
			bool PointerPosition = false;
		};
		// 推进注入相位一帧(旧的 Apply 语义:一次调用 = 一帧),写进 `out`。
		void AdvancePhase(const std::string& windowKey, WuiInputState& out, bool& outPointerPosition);
		// 把本帧注入并入调用方状态(并集:不覆盖平台轮询的其它字段)。
		static void MergeInjection(const FrameInjection& injection, WuiInputState& input);

	private:
		struct Pending
		{
			glm::vec2 Position { 0, 0 };
			int Button = 0;  // 注入的鼠标键:与 MouseDown/Clicked/Released 下标一致
			int FramesLeft = 0;
			int Phase = 0;   // 0 = press, 1 = release
			// 文本阶段(点击完成后开始):按行拆分的码点,每帧消费一段。
			std::vector<std::vector<uint32_t>> TextFrames;
			size_t NextTextFrame = 0;
			// 按键注入(0 = 无;1 = 待按下的帧;2 = 待释放的帧)。
			uint32_t Key = 0;
			int KeyPhase = 0;
			// 按下之后还要"按住"多少帧(见 QueueKey 的 holdFrames 口径)。
			int KeyHoldFrames = 0;
			bool KeyCtrl = false;
			bool KeyShift = false;
			// 点击注入的修饰键(M38 之后追加):Ctrl+点击 = 面板的"加选"(多选),
			// Shift+点击 = 范围/等比。两帧(按下 + 抬起)都保持同一份修饰状态,
			// 面板读到的 `ctx.Input().Ctrl` 因此与真人按住 Ctrl 点一下完全一致。
			bool ClickCtrl = false;
			bool ClickShift = false;
			// 滚轮注入(0 = 无;>0 = 还剩几帧要写 Wheel)。
			int WheelFrames = 0;
			float Wheel = 0.0f;
		};

		// 本帧的注入快照(键 = 窗口;BeginFrame 清空)。相位每帧只推进一次,同一帧的多个
		// 消费者读同一份 ⇒ 游戏 UI 与 WUI 控件都能看到同一次注入。
		std::unordered_map<std::string, FrameInjection> m_FrameSnapshots;
		// 帧序号:0 = 从未 BeginFrame(旧语义:一次 Apply = 一帧,破坏性推进)。
		uint64_t m_Serial = 0;
		std::unordered_map<std::string, Pending> m_Pending;
	};
}
