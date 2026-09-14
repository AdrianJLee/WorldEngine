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
		void Minimize() override;
		void MaximizeOrRestore() override;
		bool IsMaximized() const override;

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
		GLFWwindow* m_ShareWindow = nullptr;
		WNDPROC m_PrevWndProc = nullptr;
		static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
		LRESULT HitTestNc(LPARAM lParam);

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
