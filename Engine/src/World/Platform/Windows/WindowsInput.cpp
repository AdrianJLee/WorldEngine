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
}
