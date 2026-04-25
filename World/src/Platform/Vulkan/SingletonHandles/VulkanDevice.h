#pragma once
#include "VulkanPhysicalDevice.h"
#include <vk_mem_alloc.h>

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
		VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
		VkQueue GetPresentQueue() const { return m_PresentQueue; }
		VkQueue GetComputeQueue() const { return m_ComputeQueue; }

		//如果以后要做异步加载，可以暴露这个
		// TODO : 这里可以考虑使用单独的传输队列来进行资源上传，目前先使用图形队列来进行资源上传
		VkQueue GetTransferQueue() const { return m_TransferQueue; }

		VkDevice GetLogicalDevice() const { return m_LogicalDevice; }

		VmaAllocator GetAllocator() const { return m_Allocator; }

		const Ref<VulkanPhysicalDevice>& GetVulkanPhysicalDevice() const { return m_PhysicalDevice; }
	private:
		VkDevice m_LogicalDevice = VK_NULL_HANDLE;
		VmaAllocator m_Allocator = VK_NULL_HANDLE;

		std::vector<VkQueue> m_GraphicsQueues;
		std::vector<VkQueue> m_ComputeQueues;

		VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
		VkQueue m_PresentQueue = VK_NULL_HANDLE;
		VkQueue m_ComputeQueue = VK_NULL_HANDLE;
		VkQueue m_TransferQueue = VK_NULL_HANDLE;

		Ref<VulkanPhysicalDevice> m_PhysicalDevice;
	};
}