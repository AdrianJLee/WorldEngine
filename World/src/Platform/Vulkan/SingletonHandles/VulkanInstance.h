#pragma once
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>

namespace World
{
	class VulkanInstance
	{
	public:
		VulkanInstance();
		~VulkanInstance();

	public:
		VkInstance GetInstance() const { return m_Handle; }

	private:
		VkInstance m_Handle = VK_NULL_HANDLE;

	};
}