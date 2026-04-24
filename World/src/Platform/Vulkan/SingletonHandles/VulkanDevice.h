#pragma once
#include "VulkanPhysicalDevice.h"

namespace World
{
	class VulkanDevice
	{
	public:
		VulkanDevice(const Ref<VulkanPhysicalDevice>& physicalDevice);
		~VulkanDevice();

		VulkanDevice(const VulkanDevice&) = delete;
		VulkanDevice& operator=(const VulkanDevice&) = delete;

	public:
		VkDevice GetLogicalDevice() const { return m_LogicalDevice; }
		VmaAllocator GetAllocator() const { return m_Allocator; }
		VkQueue GetGraphicsQueue() const { return m_PresentQueue; }
	private:
		VkDevice m_LogicalDevice = VK_NULL_HANDLE;
		VmaAllocator m_Allocator = VK_NULL_HANDLE;

		std::vector<VkQueue> m_GraphicsQueues;
		std::vector<VkQueue> m_ComputeQueues;
		VkQueue m_PresentQueue = VK_NULL_HANDLE;

		Ref<VulkanPhysicalDevice> m_PhysicalDevice;
	};
}