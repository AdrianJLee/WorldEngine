#include "wldpch.h"
#include "VulkanCommandPool.h"

namespace World
{
	VulkanCommandPool::VulkanCommandPool(Ref<VulkanDevice> device, uint32_t queueFamilyIndex)
		: m_Device(device)
	{
		WLD_PROFILE_FUNCTION();

		VkCommandPoolCreateInfo poolInfo {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = queueFamilyIndex;
		// Allow command buffers to be individually reset, which can be useful for reusing command buffers without resetting the entire pool
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

		if (vkCreateCommandPool(m_Device->GetLogicalDevice(), &poolInfo, nullptr, &m_CommandPool) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to create Vulkan Command Pool!");
		}

		WLD_CORE_INFO("Vulkan Command Pool created for queue family index: {0}", queueFamilyIndex);
	}


	VulkanCommandPool::~VulkanCommandPool()
	{
		WLD_PROFILE_FUNCTION();
		if (m_CommandPool)
		{
			vkDestroyCommandPool(m_Device->GetLogicalDevice(), m_CommandPool, nullptr);
		}
	}
}