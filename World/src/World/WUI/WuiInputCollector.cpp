#include "wldpch.h"
#include "WuiInputCollector.h"

#include "World/Core/KeyCodes.h"

#include <GLFW/glfw3.h>

namespace World::Wui
{
	void WuiInputCollector::SyncButtonsWithSystem()
	{
		const bool left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		const bool right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
		const bool middle = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
		if (!left && m_MouseDown[0]) { m_MouseDown[0] = false; m_MouseReleased[0] = true; }
		if (!right && m_MouseDown[1]) { m_MouseDown[1] = false; m_MouseReleased[1] = true; }
		if (!middle && m_MouseDown[2]) { m_MouseDown[2] = false; m_MouseReleased[2] = true; }
	}

	namespace
	{
		bool Has(const std::unordered_set<uint32_t>& set, uint32_t key) { return set.find(key) != set.end(); }
	}

	void WuiInputCollector::OnKey(uint32_t keyCode, bool down, bool repeat)
	{
		if (down)
		{
			// 沿与重复分开锁存:控件按"一次动作"消费沿,长按由 OS 重复事件驱动。
			if (m_Down.insert(keyCode).second)
				m_Pressed.push_back(keyCode);
			else if (repeat)
				m_Repeated.push_back(keyCode);
		}
		else m_Down.erase(keyCode);
	}

	void WuiInputCollector::OnChar(uint32_t codepoint)
	{
		if (codepoint)
			m_Chars.push_back(codepoint);
	}

	void WuiInputCollector::OnMouseButton(int button, bool down)
	{
		if (button < 0 || button > 2)
			return;
		// 按键沿必须锁存:事件可能落在 BeginFrame 与 EndFrame 之间(例如拖动过程中
		// 新建/激活窗口会泵消息),此时若只靠 down/prev 逐帧比较,这一次按下/抬起
		// 会被整帧吞掉 —— 表现是拖拽状态永远结束不了、落点预览一直显示。
		if (down != m_MouseDown[button])
		{
			if (down) m_MouseClicked[button] = true;
			else m_MouseReleased[button] = true;
		}
		m_MouseDown[button] = down;
		if (down)
		{
			const double now = glfwGetTime();
			const glm::vec2 delta = m_MousePos - m_LastClickPos[button];
			if (now - m_LastClickTime[button] < 0.35 && glm::dot(delta, delta) < 36.0f)
				m_MouseDoubleClicked[button] = true;
			m_LastClickTime[button] = now;
			m_LastClickPos[button] = m_MousePos;
		}
	}

	void WuiInputCollector::OnMouseMove(float x, float y)
	{
		// P4-UX2c:输入是物理像素,面板布局是设计单位(window / UiScale) → 这里换算,
		// 否则内容缩放后所有命中都会偏移。
		const float scale = UiScale() > 0.0f ? UiScale() : 1.0f;
		m_MousePos = { x / scale, y / scale };
	}

	void WuiInputCollector::OnMouseScroll(float dx, float dy)
	{
		m_Wheel += dy;
		(void)dx;
	}

	void WuiInputCollector::BeginFrame(WuiInputState& out, glm::vec2 viewport, float fps)
	{
		SyncButtonsWithSystem();
		out.MousePos = m_MousePos;
		out.ViewportSize = viewport;
		out.FPS = fps;
		out.Wheel = m_Wheel;
		for (int i = 0; i < 3; ++i)
		{
			out.MouseDown[i] = m_MouseDown[i];
			// 锁存的沿与"逐帧比较"取并集:前者覆盖"帧中途到达"的事件,后者覆盖
			// 只更新了 m_MouseDown 没走 OnMouseButton 的路径(如系统同步)。
			out.MouseClicked[i] = m_MouseClicked[i] || (m_MouseDown[i] && !m_PrevMouseDown[i]);
			out.MouseReleased[i] = m_MouseReleased[i] || (!m_MouseDown[i] && m_PrevMouseDown[i]);
			out.MouseDoubleClicked[i] = m_MouseDoubleClicked[i];
			// 本帧交付后即消费:本帧中途才到达的事件留给下一帧,不丢。
			m_MouseClicked[i] = false;
			m_MouseReleased[i] = false;
		}
		out.KeyDown.assign(m_Down.begin(), m_Down.end());
		out.KeyPressed = m_Pressed;
		out.KeyRepeated = m_Repeated;
		m_Pressed.clear();
		m_Repeated.clear();
		out.TextInput = m_Chars;
		out.Ctrl = Has(m_Down, KeyCodes::LeftControl) || Has(m_Down, KeyCodes::RightControl);
		out.Shift = Has(m_Down, KeyCodes::LeftShift) || Has(m_Down, KeyCodes::RightShift);
		out.Alt = Has(m_Down, KeyCodes::LeftAlt) || Has(m_Down, KeyCodes::RightAlt);
		out.WantKeyboard = false;
	}

	void WuiInputCollector::EndFrame()
	{
		for (int i = 0; i < 3; ++i)
		{
			m_PrevMouseDown[i] = m_MouseDown[i];
			m_MouseDoubleClicked[i] = false;
		}
		m_Wheel = 0;
		m_Chars.clear();
	}
}
