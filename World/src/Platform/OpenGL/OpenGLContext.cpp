#include "wldpch.h"
#include "OpenGLContext.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace World
{
	OpenGLContext::OpenGLContext(GLFWwindow* windowHandle, bool initializeLoader)
		: m_WindowHandle(windowHandle), m_InitializeLoader(initializeLoader)
	{
		WLD_CORE_ASSERT(windowHandle, "Window handle is null!");
	}

	void OpenGLContext::Init()
	{
		WLD_PROFILE_FUNCTION();

		// Make the OpenGL context current
		glfwMakeContextCurrent(m_WindowHandle);
		if (!m_InitializeLoader)
			return; // 共享上下文的附加窗口:GLAD 指针由主窗口加载
		int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		WLD_CORE_ASSERT(status, "Failed to initialize GLAD!");

		WLD_CORE_INFO("OpenGL Info:");
		WLD_CORE_INFO("  Vendor: {0}", (const char*)glGetString(GL_VENDOR));
		WLD_CORE_INFO("  Renderer: {0}", (const char*)glGetString(GL_RENDERER));
		WLD_CORE_INFO("  Version: {0}", (const char*)glGetString(GL_VERSION));

	}
	void OpenGLContext::SwapBuffers()
	{
		WLD_PROFILE_FUNCTION();

		// Swap front and back buffers
		glfwSwapBuffers(m_WindowHandle);
	}
}
