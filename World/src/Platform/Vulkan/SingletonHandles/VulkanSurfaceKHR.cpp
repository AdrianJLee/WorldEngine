#include "wldpch.h"
#include "VulkanSurfaceKHR.h"

namespace World
{
	VulkanSurfaceKHR::VulkanSurfaceKHR(const Ref<VulkanInstance>& instance, GLFWwindow* windowHandle)
		: m_Instance(instance)
	{
		WLD_PROFILE_FUNCTION();
		if (glfwCreateWindowSurface(m_Instance->GetInstance(), windowHandle, nullptr, &m_Surface) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to create window surface!");
		}

		WLD_CORE_INFO("Vulkan Surface created successfully.");
	}

	VulkanSurfaceKHR::~VulkanSurfaceKHR()
	{
		WLD_PROFILE_FUNCTION();
		if (m_Surface)
		{
			vkDestroySurfaceKHR(m_Instance->GetInstance(), m_Surface, nullptr);
			WLD_CORE_INFO("Vulkan Surface destroyed.");
		}
	}
}