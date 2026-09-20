#pragma once
#include "World/Core/Window.h"

#include <GLFW/glfw3.h>

namespace World
{
	class WindowsWindow :public Window
	{
	public:
		WindowsWindow(const WindowProps& props);
		// 附加窗口:不重复初始化 GLFW/Vulkan 设备,GL 共享主窗口上下文。
		WindowsWindow(const WindowProps& props, GLFWwindow* shareWindow, bool auxiliary);
		virtual ~WindowsWindow();

		void OnUpdate() override;


		inline uint32_t GetWidth() const override { return m_Data.Width; };
		inline uint32_t GetHeight() const override { return m_Data.Height; }

		//Window attributtes
		inline void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		void SetVsync(bool enabled) override;
		bool IsVsync() const override;

		inline virtual void* GetNativeWindow() const override { return m_Window; };

		void MakeCurrent() override;
		void SwapBuffers() override;
		void SetPosition(int x, int y) override;
		void GetPosition(int* x, int* y) const override;
		void SetSize(uint32_t width, uint32_t height) override;
		bool ShouldClose() const override;
		void SetShouldClose(bool shouldClose) override;
		void Focus() override;
		void SetVisible(bool visible) override;
		void BeginSystemDrag() override;
		void SetFrameless(bool frameless) override;
		bool IsFrameless() const override;
		void Minimize() override;
		void MaximizeOrRestore() override;
		bool IsMaximized() const override;

		// W9-2:系统剪贴板(glfwGet/SetClipboardString)与窗口焦点(glfwGetWindowAttrib(GLFW_FOCUSED))。
		std::string GetClipboardText() const override;
		void SetClipboardText(const std::string& text) override;
		bool IsFocused() const override;

		// ---- D10:OS 文件拖放(资源管理器 → 窗口)----
		// 本窗口自己的队列:WndProc 收 WM_DROPFILES 时写入,这里取出并清空。
		std::vector<std::string> ConsumeDroppedFiles() override;

	private:
		virtual void Init(const WindowProps& props);
		virtual void Shutdown();
	private:
		GLFWwindow* m_Window;
		// 必须初始化为空:Vulkan 附加窗口不创建 GL 上下文,
		// 否则 Shutdown 里的 delete m_Context 会释放野指针(销毁窗口即崩)。
		class GraphicsContext* m_Context = nullptr;
		// 附加窗口:GL 共享上下文,不拥有 GLFW 初始化;Vulkan 窗口不建 GL 上下文。
		bool m_Auxiliary = false;
		bool m_HasGLContext = true;
		// 进程级"隐藏运行"(WLD_WINDOW_HIDDEN 或 SW_HIDE 启动):所有窗口都不显示,
		// 且显式 SetVisible(true) 也被吞掉(浮窗挂靠/摘出会调用它)。
		bool m_ForceHidden = false;
		GLFWwindow* m_ShareWindow = nullptr;
		WNDPROC m_PrevWndProc = nullptr;
		// D10:拖入本窗口的文件(绝对路径,UTF-8)。只由创建/销毁窗口的线程访问:
		// WM_DROPFILES 在 glfwPollEvents 里派发,ConsumeDroppedFiles 由宿主同线程调用。
		std::vector<std::string> m_DroppedFiles;
		// 无边框窗口才接管 WM_NCHITTEST(边缘缩放命中);有系统标题栏时必须交还
		// GLFW/系统,否则标题栏拖动与系统按钮会失效。
		bool m_Frameless = false;
		static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
		LRESULT HitTestNc(LPARAM lParam);
		int ResizeBorderPixels() const override;
		// D10:WM_DROPFILES:用 DragQueryFileW 逐项取路径进 m_DroppedFiles,再 DragFinish。
		LRESULT HandleDroppedFiles(WPARAM wParam);

		struct WindowData
		{
			std::string Title;
			unsigned int Width, Height;
			bool VSync;

			EventCallbackFn EventCallback;
		};

		WindowData m_Data;
	};
}
