#include "wldpch.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "World/Core/Input.h"
#include "World/Core/Application.h"

#include <GLFW/glfw3.h>
#include <unordered_map>

namespace World
{
	namespace
	{
		// 滚轮增量累积器(仅主线程读写:事件分发与帧采样都在主线程)。
		float s_ScrollX = 0.0f;
		float s_ScrollY = 0.0f;
		// 捕获态指针位移兜底累积器(同线程)。
		float s_MouseDeltaX = 0.0f;
		float s_MouseDeltaY = 0.0f;

		// 模拟按键注入通道与边沿状态追踪
		std::unordered_map<int, bool> s_SimulatedKeys;
		std::unordered_map<int, bool> s_CurrentKeyStates;
		std::unordered_map<int, bool> s_PreviousKeyStates;

		bool QueryRawPhysicalKey(int keycode)
		{
			auto it = s_SimulatedKeys.find(keycode);
			if (it != s_SimulatedKeys.end() && it->second)
				return true;

			if (Application::HasInstance())
			{
				auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
				if (window && glfwGetKey(window, keycode) == GLFW_PRESS)
					return true;

				if (Application::Get().GetWindow().IsFocused())
				{
					int vk = -1;
					if (keycode >= 65 && keycode <= 90) vk = keycode;
					else if (keycode >= 48 && keycode <= 57) vk = keycode;
					else if (keycode == 32) vk = VK_SPACE;
					else if (keycode == 256) vk = VK_ESCAPE;
					else if (keycode == 257) vk = VK_RETURN;
					else if (keycode == 258) vk = VK_TAB;
					if (vk >= 0 && (::GetAsyncKeyState(vk) & 0x8000))
						return true;
				}
			}
			return false;
		}
	}

	bool Input::IsKeyPressed(int keycode)
	{
		const bool pressed = QueryRawPhysicalKey(keycode);
		s_CurrentKeyStates[keycode] = pressed;
		return pressed;
	}

	bool Input::IsKeyJustPressed(int keycode)
	{
		const bool current = IsKeyPressed(keycode);
		const auto prevIt = s_PreviousKeyStates.find(keycode);
		const bool prev = (prevIt != s_PreviousKeyStates.end()) ? prevIt->second : false;
		return current && !prev;
	}

	bool Input::IsKeyJustReleased(int keycode)
	{
		const bool current = IsKeyPressed(keycode);
		const auto prevIt = s_PreviousKeyStates.find(keycode);
		const bool prev = (prevIt != s_PreviousKeyStates.end()) ? prevIt->second : false;
		return !current && prev;
	}

	void Input::SetSimulatedKeyState(int keycode, bool pressed)
	{
		if (pressed)
			s_SimulatedKeys[keycode] = true;
		else
			s_SimulatedKeys.erase(keycode);
		s_CurrentKeyStates[keycode] = pressed;
	}

	void Input::ClearSimulatedKeys()
	{
		s_SimulatedKeys.clear();
		s_CurrentKeyStates.clear();
		s_PreviousKeyStates.clear();
	}

	void Input::UpdateFrameInputState()
	{
		s_PreviousKeyStates = s_CurrentKeyStates;
		s_CurrentKeyStates.clear();
		for (const auto& kv : s_SimulatedKeys)
		{
			if (kv.second)
				s_CurrentKeyStates[kv.first] = true;
		}
	}

	bool Input::IsMouseButtonPressed(int button)
	{
		auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		auto state = glfwGetMouseButton(window, button);
		return state == GLFW_PRESS;
	}
	std::pair<float, float> Input::GetMousePosition()
	{
		auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		double xpos, ypos;
		glfwGetCursorPos(window, &xpos, &ypos);
		return { (float)xpos,(float)ypos };
	}
	float Input::GetMouseX()
	{
		auto [x, y] = GetMousePosition();
		return x;
	}
	float Input::GetMouseY()
	{
		auto [x, y] = GetMousePosition();
		return y;
	}



	void Input::AccumulateScroll(float xOffset, float yOffset)
	{
		s_ScrollX += xOffset;
		s_ScrollY += yOffset;
	}

	std::pair<float, float> Input::GetScrollDelta()
	{
		return { s_ScrollX, s_ScrollY };
	}

	void Input::ResetScrollDelta()
	{
		s_ScrollX = 0.0f;
		s_ScrollY = 0.0f;
	}

	void Input::AccumulateMouseDelta(float dx, float dy)
	{
		s_MouseDeltaX += dx;
		s_MouseDeltaY += dy;
	}

	std::pair<float, float> Input::ConsumeMouseDelta()
	{
		const std::pair<float, float> delta { s_MouseDeltaX, s_MouseDeltaY };
		s_MouseDeltaX = 0.0f;
		s_MouseDeltaY = 0.0f;
		return delta;
	}
}
