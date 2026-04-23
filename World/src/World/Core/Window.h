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

		WindowProps(const std::string& title = "World Engine",
			uint32_t width = 1280, uint32_t height = 720)
			: Title(title), Widdth(width), Height(height)
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

		// Creates a window
		static Window* Create(const WindowProps& props = WindowProps());

	};
}