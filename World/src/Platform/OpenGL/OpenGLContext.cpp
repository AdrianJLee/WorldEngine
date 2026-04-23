#include "wldpch.h"
#include "OpenGLContext.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

namespace World
{
	OpenGLContext::OpenGLContext(GLFWwindow* windowHandle)
		:m_WindowHandle(windowHandle)
	{
		WLD_CORE_ASSERT(windowHandle, "Window handle is null!");
	}

	void OpenGLContext::Init()
	{
		WLD_PROFILE_FUNCTION();

		// Make the OpenGL context current
		glfwMakeContextCurrent(m_WindowHandle);
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