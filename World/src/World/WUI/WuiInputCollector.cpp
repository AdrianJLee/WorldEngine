#include "wldpch.h"
#include "WuiInputCollector.h"

#include "World/Core/KeyCodes.h"

#include <GLFW/glfw3.h>

namespace World::Wui
{
	namespace
	{
		bool Has(const std::unordered_set<uint32_t>& set, uint32_t key) { return set.find(key) != set.end(); }
	}

	void WuiInputCollector::OnKey(uint32_t keyCode, bool down, bool repeat)
	{
		(void)repeat;
		if (down) m_Down.insert(keyCode);
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
		m_MousePos = { x, y };
	}

	void WuiInputCollector::OnMouseScroll(float dx, float dy)
	{
		m_Wheel += dy;
		(void)dx;
	}

	void WuiInputCollector::BeginFrame(WuiInputState& out, glm::vec2 viewport, float fps)
	{
		out.MousePos = m_MousePos;
		out.ViewportSize = viewport;
		out.FPS = fps;
		out.Wheel = m_Wheel;
		for (int i = 0; i < 3; ++i)
		{
			out.MouseDown[i] = m_MouseDown[i];
			out.MouseClicked[i] = m_MouseDown[i] && !m_PrevMouseDown[i];
			out.MouseReleased[i] = !m_MouseDown[i] && m_PrevMouseDown[i];
			out.MouseDoubleClicked[i] = m_MouseDoubleClicked[i];
		}
		out.KeyDown.assign(m_Down.begin(), m_Down.end());
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
