#include "wldpch.h"
#define VMA_IMPLEMENTATION
#include "VulkanContext.h"

namespace World
{
	VulkanContext::VulkanContext(GLFWwindow* windowHandle)
		: m_WindowHandle(windowHandle)
	{
		WLD_CORE_ASSERT(windowHandle, "Window handle is null!");
	}
	void VulkanContext::Init()
	{
		WLD_PROFILE_FUNCTION();

		m_Instance = CreateRef<VulkanInstance>();
		m_Surface = CreateRef<VulkanSurfaceKHR>(m_Instance, m_WindowHandle);
		m_PhysicalDevice = CreateRef<VulkanPhysicalDevice>(m_Instance, m_Surface);
		m_Device = CreateRef<VulkanDevice>(m_PhysicalDevice);
	}
	void VulkanContext::SwapBuffers()
	{
		// Vulkan does not use traditional buffer swapping like OpenGL.
		// Instead, it uses a more complex presentation model involving command buffers and swapchains.
	}
}