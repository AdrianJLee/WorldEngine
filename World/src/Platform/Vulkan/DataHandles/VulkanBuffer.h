#pragma once
#include "Platform/Vulkan/SingletonHandles/VulkanDevice.h"

namespace World
{
	class VulkanBuffer
	{
	public:
		VulkanBuffer(Ref<VulkanDevice> device, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage);
		~VulkanBuffer();

		VulkanBuffer(const VulkanBuffer&) = delete;
		VulkanBuffer& operator=(const VulkanBuffer&) = delete;

		void Map();
		void Unmap();
		void WriteToBuffer(void* data, VkDeviceSize size, VkDeviceSize offset = 0);

		VkBuffer GetHandle() const { return m_Buffer; }
		VkDeviceSize GetSize() const { return m_Size; }

	private:
		Ref<VulkanDevice> m_Device;
		VkBuffer m_Buffer = VK_NULL_HANDLE;
		VmaAllocation m_Allocation = VK_NULL_HANDLE;
		VkDeviceSize m_Size;
		void* m_MappedData = nullptr;
	};
}