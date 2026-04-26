#include "wldpch.h"
#include "VulkanBuffer.h"

namespace World
{
	VulkanBuffer::VulkanBuffer(Ref<VulkanDevice> device, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memUsage)
		: m_Device(device), m_Size(size)
	{
		WLD_PROFILE_FUNCTION();

		VkBufferCreateInfo bufferInfo { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bufferInfo.size = size;
		bufferInfo.usage = usage;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo {};
		allocInfo.usage = memUsage;

		if (memUsage == VMA_MEMORY_USAGE_CPU_ONLY || memUsage == VMA_MEMORY_USAGE_CPU_TO_GPU)
		{
			allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		}

		if (vmaCreateBuffer(m_Device->GetAllocator(), &bufferInfo, &allocInfo, &m_Buffer, &m_Allocation, nullptr) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to create Vulkan Buffer via VMA!");
		}
	}
	VulkanBuffer::~VulkanBuffer()
	{
		WLD_PROFILE_FUNCTION();
		if (m_Buffer)
		{
			vmaDestroyBuffer(m_Device->GetAllocator(), m_Buffer, m_Allocation);
		}
	}
	void VulkanBuffer::Map()
	{
		WLD_PROFILE_FUNCTION();

		vmaMapMemory(m_Device->GetAllocator(), m_Allocation, &m_MappedData);
	}
	void VulkanBuffer::Unmap()
	{
		WLD_PROFILE_FUNCTION();

		vmaUnmapMemory(m_Device->GetAllocator(), m_Allocation);
		m_MappedData = nullptr;
	}
	void VulkanBuffer::WriteToBuffer(void* data, VkDeviceSize size, VkDeviceSize offset)
	{
		WLD_PROFILE_FUNCTION();

		WLD_CORE_ASSERT(m_MappedData, "Buffer must be mapped before writing!");
		memcpy((uint8_t*)m_MappedData + offset, data, size);
	}
}