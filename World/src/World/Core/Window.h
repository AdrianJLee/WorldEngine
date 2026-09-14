#pragma once
#include "wldpch.h"

#include "World/Core/Core.h"
#include "World/Events/Event.h"

namespace World
{
	struct WindowProps
	{
		std::string Title;
		uint32_t Widdth;
		uint32_t Height;
		bool Frameless = false;

		WindowProps(const std::string& title = "World Engine",
			uint32_t width = 1280, uint32_t height = 720, bool frameless = false)
			: Title(title), Widdth(width), Height(height), Frameless(frameless)
		{

		}
	};

	class Window
	{
	public:
		//函数容器
		using EventCallbackFn = std::function<void(Event&)>;

		virtual ~Window() {}

		virtual void OnUpdate() = 0;

		virtual uint32_t GetWidth() const = 0;

		virtual uint32_t GetHeight() const = 0;

		// Window attributes

		// Sets the event callback function
		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
		// Sets the VSync state
		virtual void SetVsync(bool enabled) = 0;
		// Gets the VSync state
		virtual bool IsVsync() const = 0;


		virtual void* GetNativeWindow() const = 0;

		// ---- 多窗口(独立浮动窗口)支持 ----
		// GL:把该窗口的上下文设为当前;其他后端为 no-op。
		virtual void MakeCurrent() = 0;
		// GL:交换该窗口的缓冲;Vulkan 由 present target 负责。
		virtual void SwapBuffers() = 0;
		virtual void SetPosition(int x, int y) = 0;
		virtual void GetPosition(int* x, int* y) const = 0;
		virtual void SetSize(uint32_t width, uint32_t height) = 0;
		virtual bool ShouldClose() const = 0;
		virtual void SetShouldClose(bool shouldClose) = 0;
		virtual void Focus() = 0;
		// 无边框窗口自定义标题栏:让系统进入"窗口移动"循环。
		virtual void BeginSystemDrag() = 0;

		// Creates a window
		static Window* Create(const WindowProps& props = WindowProps());
		// 创建共享主窗口图形资源(GL 共享上下文 / Vulkan 同设备)的附加窗口。
		static Window* CreateAuxiliary(const WindowProps& props, Window* share);

	};
}
