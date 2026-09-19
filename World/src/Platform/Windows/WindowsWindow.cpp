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
#include <shellapi.h>

// D10:OS 文件拖放(DragAcceptFiles / DragQueryFileW / DragFinish,WM_DROPFILES)。
// World 的 CMake 链接表里没有 shell32(现有的 SHBrowseForFolderW 依赖 Editor 侧
// ContentBrowserPanel.cpp 的同类 pragma),本文件自带这一条,Runtime/Tests 链接到
// WindowsWindow.obj 时也能解析。若改为在 CMake 里显式链接 shell32,可删掉这一行。
#pragma comment(lib, "shell32.lib")

namespace World
{
	static bool s_GLFWInitialized = false;

	static void GLFWErrorCallback(int error, const char* description)
	{
		WLD_CORE_ERROR("GLFW Error ({0}): {1}", error, description);
	}

	// D10:WM_DROPFILES 给的是 UTF-16 路径,引擎内部资产路径统一用 UTF-8 std::string
	// (与 WindowsPlatformUtils.cpp 里 WideCharToMultiByte(CP_UTF8, …) 的用法一致)。
	static std::string WideToUtf8(const std::wstring& wide)
	{
		if (wide.empty())
			return std::string();
		const int sourceLength = static_cast<int>(wide.size());
		const int length = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), sourceLength,
			nullptr, 0, nullptr, nullptr);
		if (length <= 0)
			return std::string();
		std::string result(static_cast<size_t>(length), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), sourceLength,
			result.data(), length, nullptr, nullptr);
		return result;
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
			// 后台/自动化运行:窗口是否可见由本进程直接决定,而不是只依赖启动器的
			// -WindowStyle Hidden(GLFW 默认仍会显示窗口,实测会让验证时弹窗到桌面)。
			// 判定顺序:WLD_WINDOW_HIDDEN 环境变量 > 进程被以 SW_HIDE 启动。
			bool hidden = false;
			if (const char* env = std::getenv("WLD_WINDOW_HIDDEN"))
				hidden = env[0] != '0';
			if (!hidden)
			{
				STARTUPINFOW startupInfo{};
				GetStartupInfoW(&startupInfo);
				hidden = (startupInfo.dwFlags & STARTF_USESHOWWINDOW) != 0
					&& startupInfo.wShowWindow == SW_HIDE;
			}
			if (hidden)
				glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
			m_ForceHidden = hidden;
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
		// D10:OS 文件拖放。GLFW 建窗时已经 DragAcceptFiles(TRUE) 并放行了 WM_DROPFILES
		// 的 UIPI 过滤,但它的窗口过程只把 WM_DROPFILES 转给 drop callback(引擎没设),
		// 路径会被丢掉。这里给每个窗口(主窗口/独立窗口各一份)装自己的窗口过程,
		// 拦截 WM_DROPFILES 存进本窗口的队列;其余消息原样转发给 GLFW 的过程。
		// GWLP_USERDATA 归我们使用是安全的:GLFW 用 SetPropW(hwnd, L"GLFW", …) 存取自己
		// 的窗口指针,不占用 GWLP_USERDATA。无边框窗口本来就走这套机制,现在改成建窗即装。
		if (const HWND hwnd = glfwGetWin32Window(m_Window))
		{
			m_Frameless = props.Frameless;
			m_DroppedFiles.clear();
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
			m_PrevWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
				hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowsWindow::StaticWndProc)));
			DragAcceptFiles(hwnd, TRUE);
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
				if (data.EventCallback) data.EventCallback(event);

			});

		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				WindowCloseEvent event;
				if (data.EventCallback) data.EventCallback(event);
			});

		glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int scancode, int action, int mods)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				switch (action)
				{
					case GLFW_PRESS:
					{
						KeyPressedEvent event(key, 0);
						if (data.EventCallback) data.EventCallback(event);
						break;
					}
					case GLFW_RELEASE:
					{
						KeyReleasedEvent event(key);
						if (data.EventCallback) data.EventCallback(event);
						break;
					}
					case GLFW_REPEAT:
					{
						KeyPressedEvent event(key, 1);
						if (data.EventCallback) data.EventCallback(event);
						break;
					}
				}
			});

		//当用户输入了一个完整的字符时，请立刻通过这个匿名函数通知
		glfwSetCharCallback(m_Window, [](GLFWwindow* window, unsigned int keycode)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				KeyTypedEvent event(keycode);
				if (data.EventCallback) data.EventCallback(event);
			});

		glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, int mods)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				switch (action)
				{
					case GLFW_PRESS:
					{
						MouseButtonPressedEvent event(button);
						if (data.EventCallback) data.EventCallback(event);
						break;
					}
					case GLFW_RELEASE:
					{
						MouseButtonReleasedEvent event(button);
						if (data.EventCallback) data.EventCallback(event);
						break;
					}
				}
			});

		glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double xOffset, double yOffset)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				MouseScrolledEvent event((float)xOffset, (float)yOffset);
				if (data.EventCallback) data.EventCallback(event);
			});

		glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double xPos, double yPos)
			{
				WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
				MouseMovedEvent event((float)xPos, (float)yPos);
				if (data.EventCallback) data.EventCallback(event);
			});

	}

	void WindowsWindow::Shutdown()
	{
		WLD_PROFILE_FUNCTION();

		// 第三方输入法(如搜狗)会在进程退出时由系统回调其清理代码并可能崩溃。
		// 在销毁窗口前禁用线程 IME 并泵空消息,让输入法先完成解挂。
		// 注意:仅主窗口这么做。附加窗口的销毁发生在主窗口的一帧渲染内部,
		// 此时泵消息会把消息重入分发回主窗口(正在渲染/正在改容器),导致崩溃。
		const bool skipIme = std::getenv("WLD_SKIP_IME_SHUTDOWN") != nullptr;
		if (!skipIme && !m_Auxiliary)
		{
			if (const HWND hwnd = glfwGetWin32Window(m_Window))
			{
				ImmAssociateContext(hwnd, nullptr);
				ImmDisableIME(GetCurrentThreadId());
			}
		}
		MSG message {};
		if (!m_Auxiliary)
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}

		delete m_Context;
		m_Context = nullptr;

		// D10:销毁前摘掉自己的窗口过程(WM_DROPFILES 处理)并停止接收拖放,同时清掉
		// GWLP_USERDATA,避免销毁过程中的消息再按 this 解析。
		if (m_PrevWndProc)
		{
			if (const HWND hwnd = glfwGetWin32Window(m_Window))
			{
				DragAcceptFiles(hwnd, FALSE);
				SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_PrevWndProc));
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
			}
			m_PrevWndProc = nullptr;
		}

		glfwDestroyWindow(m_Window);
		m_Window = nullptr;

		if (!m_Auxiliary)
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

	void WindowsWindow::SetVisible(bool visible)
	{
		if (!m_Window)
			return;
		if (m_ForceHidden && visible)
			return; // 隐藏运行:不允许任何窗口被显式显示(自动化的窗口枚举/抓图依赖这一点)
		if (visible)
			glfwShowWindow(m_Window);
		else
			glfwHideWindow(m_Window);
	}

	void WindowsWindow::BeginSystemDrag()
	{
		if (const HWND hwnd = glfwGetWin32Window(m_Window))
		{
			ReleaseCapture();
			// SC_MOVE 是无边框窗口最可靠的标准拖动方式(WM_NCLBUTTONDOWN+HTCAPTION
			// 会被 GLFW 的窗口过程吞掉,导致无边框窗口拖不动)。
			SendMessageW(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
		}
	}

	void WindowsWindow::SetFrameless(bool frameless)
	{
		if (!m_Window)
			return;
		glfwSetWindowAttrib(m_Window, GLFW_DECORATED, frameless ? GLFW_FALSE : GLFW_TRUE);
		// D10:窗口过程在 Init 里就装好了(要收 WM_DROPFILES),不再随无边框开关装卸;
		// 这里只切换 StaticWndProc 是否接管 WM_NCHITTEST(无边框的边缘缩放命中)。
		// GLFW 切换 GLFW_DECORATED 只改窗口样式(updateWindowStyles),不会重建 HWND,
		// 所以子类过程与 DragAcceptFiles 都保持有效。
		m_Frameless = frameless;
	}

	bool WindowsWindow::IsFrameless() const
	{
		if (!m_Window)
			return false;
		return glfwGetWindowAttrib(m_Window, GLFW_DECORATED) == GLFW_FALSE;
	}

	void WindowsWindow::Minimize()
	{
		if (m_Window)
			glfwIconifyWindow(m_Window);
	}

	void WindowsWindow::MaximizeOrRestore()
	{
		if (!m_Window)
			return;
		if (glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED))
			glfwRestoreWindow(m_Window);
		else
			glfwMaximizeWindow(m_Window);
	}

	bool WindowsWindow::IsMaximized() const
	{
		return m_Window && glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED);
	}

	// ---- W9-2:系统剪贴板与窗口焦点 ----
	// GLFW 3.4 自带跨平台的剪贴板实现(Windows 后端走 CF_UNICODETEXT)。
	// 无窗口(构造失败/已销毁)时返回空串,不抛异常:调用方(脚本编辑器)按"无可粘贴内容"处理。
	std::string WindowsWindow::GetClipboardText() const
	{
		if (!m_Window)
			return std::string();
		const char* text = glfwGetClipboardString(m_Window);
		return text ? std::string(text) : std::string();
	}

	void WindowsWindow::SetClipboardText(const std::string& text)
	{
		if (!m_Window)
			return;
		glfwSetClipboardString(m_Window, text.c_str());
	}

	bool WindowsWindow::IsFocused() const
	{
		if (!m_Window)
			return false;
		return glfwGetWindowAttrib(m_Window, GLFW_FOCUSED) != 0;
	}

	// ---- D10:OS 文件拖放(资源管理器 → 窗口) ----
	std::vector<std::string> WindowsWindow::ConsumeDroppedFiles()
	{
		std::vector<std::string> files;
		files.swap(m_DroppedFiles);
		return files;
	}

	LRESULT WindowsWindow::HandleDroppedFiles(WPARAM wParam)
	{
		HDROP drop = reinterpret_cast<HDROP>(wParam);
		if (!drop)
			return 0;
		const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		for (UINT index = 0; index < count; ++index)
		{
			const UINT length = DragQueryFileW(drop, index, nullptr, 0);
			if (length == 0)
				continue;
			// 先按返回长度 +1 分配(含结尾 '\0'),再把 DragQueryFileW 实际写入的
			// 字符数(不含 '\0')缩回去,最后转 UTF-8 存进本窗口队列。
			std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
			const UINT copied = DragQueryFileW(drop, index, wide.data(), length + 1);
			if (copied == 0)
				continue;
			wide.resize(copied);
			m_DroppedFiles.push_back(WideToUtf8(wide));
		}
		// 必须调用:否则拖放源一直等待,且 HDROP 泄漏。
		DragFinish(drop);
		return 0;
	}

	LRESULT CALLBACK WindowsWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		WindowsWindow* self = reinterpret_cast<WindowsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (self)
		{
			// D10:拖入本窗口的文件存进本窗口队列(消费掉,不转给 GLFW:它只会调用
			// 引擎没设的 drop callback,并把 HDROP 释放掉)。
			if (msg == WM_DROPFILES)
				return self->HandleDroppedFiles(wParam);
			// 只有无边框窗口接管命中测试;有系统标题栏的窗口必须让 GLFW/系统决定
			// HTCAPTION/HTCLOSE 等,否则标题栏拖动与系统按钮失效。
			if (msg == WM_NCHITTEST && self->m_Frameless)
				return self->HitTestNc(lParam);
		}
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
