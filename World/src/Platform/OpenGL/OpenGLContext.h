#pragma once
#include "World/Renderer/GraphicsContext.h"

struct GLFWwindow;
namespace World
{
	class OpenGLContext :public GraphicsContext
	{
	public:
		// initializeLoader=false 用于共享上下文的附加窗口:函数指针已由主窗口加载。
		OpenGLContext(GLFWwindow* windowHandle, bool initializeLoader = true);
		virtual ~OpenGLContext() = default;
		virtual void Init() override;
		virtual void SwapBuffers() override;
	private:
		GLFWwindow* m_WindowHandle;
		bool m_InitializeLoader = true;
	};
}
