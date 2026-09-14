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
		void SyncButtonsWithSystem();

	private:
		std::unordered_set<uint32_t> m_Down;
		bool m_PrevMouseDown[3] = { false, false, false };
		bool m_MouseDown[3] = { false, false, false };
		bool m_MouseClicked[3] = { false, false, false };
		bool m_MouseReleased[3] = { false, false, false };
		bool m_MouseDoubleClicked[3] = { false, false, false };
		double m_LastClickTime[3] = { -1, -1, -1 };
		glm::vec2 m_LastClickPos[3] = {};
		glm::vec2 m_MousePos {};
		float m_Wheel = 0;
		std::vector<uint32_t> m_Chars;
	};
}
