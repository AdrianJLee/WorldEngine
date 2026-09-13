#include "wldpch.h"
#include "WindowsWindow.h"

#include "World/Events/KeyEvent.h"
#include "World/Events/MouseEvent.h"
#include "World/Events/ApplicationEvent.h"

#include "Platform/OpenGL/OpenGLContext.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <imm.h>

namespace World
{
	static bool s_GLFWInitialized = false;

	static void GLFWErrorCallback(int error, const char* description)
	{
		WLD_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
	}

	Window* Window::Create(const WindowProps& props)
	{
		return new WindowsWindow(props);
	}

	WindowsWindow::WindowsWindow(const WindowProps& props)
	{
		Init(props);
	}

	WindowsWindow::~WindowsWindow()
	{
		Shutdown();
	}

	void WindowsWindow::Init(const WindowProps& props)
	{
		WLD_PROFILE_FUNCTION();

		m_Data.Title = props.Title;
		m_Data.Width = props.Widdth;
		m_Data.Height = props.Height;

		WLD_CORE_INFO("Creating window {0} ({1}, {2})", props.Title, props.Widdth, props.Height);

		if (!s_GLFWInitialized)
		{
			{
				WLD_PROFILE_SCOPE("glfwInit");
				//TODO: glfwTerminate on shutdown
				int success = glfwInit();

				WLD_CORE_ASSERT(success, "Could not initialize GLFW!");
			}
			glfwSetErrorCallback(GLFWErrorCallback);
			s_GLFWInitialized = true;
		}

		{
			WLD_PROFILE_SCOPE("glfwCreateWindow");
			m_Window = glfwCreateWindow((int)props.Widdth, (int)props.Height, m_Data.Title.c_str(), nullptr, nullptr);
		}
		m_Context = new OpenGLContext(m_Window);
		m_Context->Init();

		// Set the user pointer to our window data
		glfwSetWindowUserPointer(m_Window, &m_Data);
		SetVsync(false);

		// Set GLFW callbacks here (not implemented in this snippet)
		glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int width, int height)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				data.Width = width;
				data.Height = height;

				WindowResizeEvent event(width, height);
				// event作为EventCallback存储函数的参数
				data.EventCallback(event);

			});

		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				WindowCloseEvent event;
				data.EventCallback(event);
			});

		glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int scancode, int action, int mods)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				switch (action)
				{
					case GLFW_PRESS:
					{
						KeyPressedEvent event(key, 0);
						data.EventCallback(event);
						break;
					}
					case GLFW_RELEASE:
					{
						KeyReleasedEvent event(key);
						data.EventCallback(event);
						break;
					}
					case GLFW_REPEAT:
					{
						KeyPressedEvent event(key, 1);
						data.EventCallback(event);
						break;
					}
				}
			});

		//当用户输入了一个完整的字符时，请立刻通过这个匿名函数通知
		glfwSetCharCallback(m_Window, [](GLFWwindow* window, unsigned int keycode)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				KeyTypedEvent event(keycode);
				data.EventCallback(event);
			});

		glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, int mods)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				switch (action)
				{
					case GLFW_PRESS:
					{
						MouseButtonPressedEvent event(button);
						data.EventCallback(event);
						break;
					}
					case GLFW_RELEASE:
					{
						MouseButtonReleasedEvent event(button);
						data.EventCallback(event);
						break;
					}
				}
			});

		glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double xOffset, double yOffset)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				MouseScrolledEvent event((float)xOffset, (float)yOffset);
				data.EventCallback(event);
			});

		glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double xPos, double yPos)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				MouseMovedEvent event((float)xPos, (float)yPos);
				data.EventCallback(event);
			});

	}

	void WindowsWindow::Shutdown()
	{
		WLD_PROFILE_FUNCTION();

		// 第三方输入法(如搜狗)会在进程退出时由系统回调其清理代码并可能崩溃。
		// 在销毁窗口前禁用线程 IME 并泵空消息,让输入法先完成解挂。
		if (const HWND hwnd = glfwGetWin32Window(m_Window))
		{
			ImmAssociateContext(hwnd, nullptr);
			ImmDisableIME(GetCurrentThreadId());
		}
		MSG message {};
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}

		glfwDestroyWindow(m_Window);

		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}

	void WindowsWindow::OnUpdate()
	{
		WLD_PROFILE_FUNCTION();

		// Poll for and process events
		glfwPollEvents();

		m_Context->SwapBuffers();
	}

	void WindowsWindow::SetVsync(bool enabled)
	{
		WLD_PROFILE_FUNCTION();

		if (enabled)
			glfwSwapInterval(1);
		else
			glfwSwapInterval(0);

		m_Data.VSync = enabled;
	}

	bool WindowsWindow::IsVsync() const
	{
		return m_Data.VSync;
	}
}
