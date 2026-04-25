#pragma once
#include "VulkanInstance.h"

namespace World
{
	class VulkanSurfaceKHR
	{
	public:
		VulkanSurfaceKHR(const Ref<VulkanInstance>& instance, GLFWwindow* windowHandle);
		~VulkanSurfaceKHR();

		VulkanSurfaceKHR(const VulkanSurfaceKHR&) = delete;
		VulkanSurfaceKHR& operator=(const VulkanSurfaceKHR&) = delete;

	public:
		VkSurfaceKHR GetSurface() const { return m_Surface; }
	private:
		VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
		Ref<VulkanInstance> m_Instance;
	};
}