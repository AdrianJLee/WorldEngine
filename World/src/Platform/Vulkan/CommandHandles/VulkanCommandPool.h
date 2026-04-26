#pragma once
#include "Platform/Vulkan/SingletonHandles/VulkanDevice.h"

namespace World
{
	class VulkanCommandPool
	{
	public:
		VulkanCommandPool(Ref<VulkanDevice> device, uint32_t queueFamilyIndex);
		~VulkanCommandPool();

		VulkanCommandPool(const VulkanCommandPool&) = delete;
		VulkanCommandPool& operator=(const VulkanCommandPool&) = delete;

		VkCommandPool GetHandle() const { return m_CommandPool; }

	private:
		VkCommandPool m_CommandPool = VK_NULL_HANDLE;
		Ref<VulkanDevice> m_Device;
	};
}