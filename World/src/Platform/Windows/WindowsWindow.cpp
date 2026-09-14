#include "wldpch.h"
#include "WindowsWindow.h"

#include "World/Events/KeyEvent.h"
#include "World/Events/MouseEvent.h"
#include "World/Events/ApplicationEvent.h"

#include "Platform/OpenGL/OpenGLContext.h"
#include "World/Renderer/Renderer.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <imm.h>
#include <dwmapi.h>

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

	Window* Window::CreateAuxiliary(const WindowProps& props, Window* share)
	{
		GLFWwindow* shareWindow = share ? static_cast<GLFWwindow*>(share->GetNativeWindow()) : nullptr;
		return new WindowsWindow(props, shareWindow, true);
	}

	WindowsWindow::WindowsWindow(const WindowProps& props)
	{
		Init(props);
	}

	WindowsWindow::WindowsWindow(const WindowProps& props, GLFWwindow* shareWindow, bool auxiliary)
		: m_Auxiliary(auxiliary), m_ShareWindow(shareWindow)
	{
		// Vulkan 模式下附加窗口不建 GL 上下文(交换链由 RHI 的 present target 创建)。
		m_HasGLContext = Renderer::GetBackendName() != "vulkan";
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
			if (!m_Auxiliary)
				s_GLFWInitialized = true;
		}

		{
			WLD_PROFILE_SCOPE("glfwCreateWindow");
			if (props.Frameless)
				glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
			// Vulkan 附加窗口用 GLFW_NO_API:该模式下 GLFW 要求 share 必须为空。
			GLFWwindow* shareWindow = (m_Auxiliary && m_HasGLContext) ? m_ShareWindow : nullptr;
			if (m_Auxiliary && !m_HasGLContext)
				glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			m_Window = glfwCreateWindow((int)props.Widdth, (int)props.Height, m_Data.Title.c_str(), nullptr, shareWindow);
			if (props.Frameless)
				glfwWindowHint(GLFW_DECORATED, GLFW_TRUE); // hint 会残留,恢复默认
			if (m_Auxiliary && !m_HasGLContext)
				glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API); // hint 会残留,恢复默认
		}
		// 深色标题栏:消除顶部白色系统标题栏,与 WUI 深色主题一致。
		if (const HWND hwnd = glfwGetWin32Window(m_Window))
		{
			const BOOL dark = TRUE;
			DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
		}
		if (m_HasGLContext)
		{
			m_Context = new OpenGLContext(m_Window, !m_Auxiliary);
			m_Context->Init();
		}

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

		delete m_Context;
		m_Context = nullptr;
		glfwDestroyWindow(m_Window);
		m_Window = nullptr;

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

		// Vulkan 由交换链 Present 呈现;GL 保持 glfwSwapBuffers。
		if (m_Context && Renderer::GetBackendName() != "vulkan")
		{
			// 多窗口:独立窗口渲染会切换当前上下文,交换前必须切回本窗口。
			glfwMakeContextCurrent(m_Window);
			m_Context->SwapBuffers();
		}
	}

	void WindowsWindow::SetVsync(bool enabled)
	{
		WLD_PROFILE_FUNCTION();
		// 无 GL 上下文的窗口(Vulkan 附加窗口)不能调用 glfwSwapInterval。
		if (!m_HasGLContext)
		{
			m_Data.VSync = false;
			return;
		}

		if (enabled)
			glfwSwapInterval(1);
		else
			glfwSwapInterval(0);

		m_Data.VSync = enabled;
	}

	void WindowsWindow::MakeCurrent()
	{
		// Vulkan 附加窗口用 GLFW_NO_API 创建,没有 GL 上下文。
		if (m_HasGLContext)
			glfwMakeContextCurrent(m_Window);
	}

	void WindowsWindow::SwapBuffers()
	{
		// Vulkan 由 present target 呈现;GL 交换该窗口的缓冲。
		if (m_HasGLContext)
			glfwSwapBuffers(m_Window);
	}

	void WindowsWindow::SetPosition(int x, int y)
	{
		glfwSetWindowPos(m_Window, x, y);
	}

	void WindowsWindow::GetPosition(int* x, int* y) const
	{
		int px = 0, py = 0;
		glfwGetWindowPos(m_Window, &px, &py);
		if (x) *x = px;
		if (y) *y = py;
	}

	void WindowsWindow::SetSize(uint32_t width, uint32_t height)
	{
		glfwSetWindowSize(m_Window, static_cast<int>(width), static_cast<int>(height));
	}

	bool WindowsWindow::ShouldClose() const
	{
		return glfwWindowShouldClose(m_Window) != 0;
	}

	void WindowsWindow::SetShouldClose(bool shouldClose)
	{
		glfwSetWindowShouldClose(m_Window, shouldClose ? GLFW_TRUE : GLFW_FALSE);
	}

	void WindowsWindow::Focus()
	{
		glfwFocusWindow(m_Window);
	}

	void WindowsWindow::BeginSystemDrag()
	{
		if (const HWND hwnd = glfwGetWin32Window(m_Window))
		{
			ReleaseCapture();
			SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
		}
	}

	void WindowsWindow::SetFrameless(bool frameless)
	{
		if (!m_Window)
			return;
		glfwSetWindowAttrib(m_Window, GLFW_DECORATED, frameless ? GLFW_FALSE : GLFW_TRUE);
		const HWND hwnd = glfwGetWin32Window(m_Window);
		if (!hwnd)
			return;
		if (frameless && !m_PrevWndProc)
		{
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
			m_PrevWndProc = reinterpret_cast<WNDPROC>(
				SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowsWindow::StaticWndProc)));
		}
		else if (!frameless && m_PrevWndProc)
		{
			SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_PrevWndProc));
			m_PrevWndProc = nullptr;
		}
	}

	LRESULT CALLBACK WindowsWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		WindowsWindow* self = reinterpret_cast<WindowsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (self && msg == WM_NCHITTEST)
			return self->HitTestNc(lParam);
		const WNDPROC previous = self ? self->m_PrevWndProc : nullptr;
		return previous ? CallWindowProcW(previous, hwnd, msg, wParam, lParam)
			: DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	LRESULT WindowsWindow::HitTestNc(LPARAM lParam)
	{
		// 无边框窗口仍保留四边/四角的缩放命中,其余交还客户区(菜单/挂靠栏拖动)。
		const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
		const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
		RECT rect {};
		GetWindowRect(glfwGetWin32Window(m_Window), &rect);
		constexpr int border = 6;
		const bool left = x < rect.left + border;
		const bool right = x >= rect.right - border;
		const bool top = y < rect.top + border;
		const bool bottom = y >= rect.bottom - border;
		if (left && top) return HTTOPLEFT;
		if (right && top) return HTTOPRIGHT;
		if (left && bottom) return HTBOTTOMLEFT;
		if (right && bottom) return HTBOTTOMRIGHT;
		if (left) return HTLEFT;
		if (right) return HTRIGHT;
		if (top) return HTTOP;
		if (bottom) return HTBOTTOM;
		return HTCLIENT;
	}

	bool WindowsWindow::IsVsync() const
	{
		return m_Data.VSync;
	}
}
