#include "wldpch.h"
#include "World/Core/Input.h"
#include "World/Core/Application.h"

#include <GLFW/glfw3.h>

namespace World
{
	bool Input::IsKeyPressed(int keycode)
	{
		auto window = static_cast<GLFWwindow*>(Application::Get().GetWindow().GetNativeWindow());
		auto state = glfwGetKey(window, keycode);
		return state == GLFW_PRESS;
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

	namespace
	{
		// 滚轮增量累积器(仅主线程读写:事件分发与帧采样都在主线程)。
		float s_ScrollX = 0.0f;
		float s_ScrollY = 0.0f;
		// 捕获态指针位移兜底累积器(同线程)。
		float s_MouseDeltaX = 0.0f;
		float s_MouseDeltaY = 0.0f;
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
