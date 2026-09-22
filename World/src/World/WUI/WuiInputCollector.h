#pragma once

#include "World/WUI/WuiContext.h"

#include <unordered_set>

namespace World::Wui
{
	// GLFW 事件 → WUI 输入态收集器。主线程每帧调用 BeginFrame/EndFrame。
	class WuiInputCollector
	{
	public:
		void OnKey(uint32_t keyCode, bool down, bool repeat);
		void OnChar(uint32_t codepoint);
		void OnMouseButton(int button, bool down);
		void OnMouseMove(float x, float y);
		void OnMouseScroll(float dx, float dy);

		void BeginFrame(WuiInputState& out, glm::vec2 viewport, float fps);
		void EndFrame();
		// 与系统按键状态同步:跨窗口拖拽时释放事件可能不会送达原窗口,
		// 此时若本窗口仍认为按钮按着,会产生"幽灵拖拽"。
		//
		// U23:同步只收口**由系统按键状态建立**的按下(真实输入)。脚本/AI 通道注入的
		// 按下(如 PostMessage 过来的 WM_LBUTTONDOWN:按下那一刻 GetAsyncKeyState 是抬起的)
		// 属于"虚拟按键",在显式抬起或超时前不会被这里清掉 —— 这是"跨帧按住 + 位移"
		// 能模拟真拖拽(材质/模型/预制体预览的左键轨道)的前提;没有注入时行为不变。
		void SyncButtonsWithSystem();

	private:
		// 注入按下的安全上限:显式抬起是正常收口,超时只兜底"注入侧崩了/释放丢了",
		// 避免虚拟按键永久按住(用户在真实鼠标上的操作不受影响)。
		static constexpr double kInjectedHoldTimeoutSeconds = 5.0;

		std::unordered_set<uint32_t> m_Down;
		bool m_PrevMouseDown[3] = { false, false, false };
		bool m_MouseDown[3] = { false, false, false };
		bool m_MouseClicked[3] = { false, false, false };
		bool m_MouseReleased[3] = { false, false, false };
		bool m_MouseDoubleClicked[3] = { false, false, false };
		// 该键当前的按下是否被系统按键状态确认(真实输入);未确认 = 虚拟按键。
		bool m_PressFromSystem[3] = { false, false, false };
		double m_PressTime[3] = { -1.0, -1.0, -1.0 };
		double m_LastClickTime[3] = { -1, -1, -1 };
		glm::vec2 m_LastClickPos[3] = {};
		glm::vec2 m_MousePos {};
		float m_Wheel = 0;
		std::vector<uint32_t> m_Chars;
		std::vector<uint32_t> m_Pressed;   // 本帧新按下(沿)
		std::vector<uint32_t> m_Repeated;  // 本帧 OS 重复事件
	};
}
