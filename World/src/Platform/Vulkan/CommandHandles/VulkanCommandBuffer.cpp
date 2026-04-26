#include "wldpch.h"
#include "VulkanCommandBuffer.h"

namespace World
{
	VulkanCommandBuffer::VulkanCommandBuffer(Ref<VulkanDevice> device, Ref<VulkanCommandPool> commandPool, uint32_t count)
		: m_Device(device), m_CommandPool(commandPool)
	{
		WLD_PROFILE_FUNCTION();

		m_CommandBuffers.resize(count);

		VkCommandBufferAllocateInfo allocInfo {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = m_CommandPool->GetHandle();
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = count;

		if (vkAllocateCommandBuffers(m_Device->GetLogicalDevice(), &allocInfo, m_CommandBuffers.data()) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to allocate Vulkan Command Buffers!");
		}
	}
	VulkanCommandBuffer::~VulkanCommandBuffer()
	{
		WLD_PROFILE_FUNCTION();
		vkFreeCommandBuffers(m_Device->GetLogicalDevice(), m_CommandPool->GetHandle(),
			static_cast<uint32_t>(m_CommandBuffers.size()), m_CommandBuffers.data());
	}
	void VulkanCommandBuffer::Begin(uint32_t index)
	{
		WLD_PROFILE_FUNCTION();

		VkCommandBufferBeginInfo beginInfo {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		// 资深写法：ONE_TIME_SUBMIT 告诉驱动我们录完一次就提交一次，利于驱动内部优化
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		vkBeginCommandBuffer(m_CommandBuffers[index], &beginInfo);
	}
	void VulkanCommandBuffer::End(uint32_t index)
	{
		WLD_PROFILE_FUNCTION();
		if (vkEndCommandBuffer(m_CommandBuffers[index]) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to record command buffer!");
		}
	}
	void VulkanCommandBuffer::Reset(uint32_t index, bool releaseResources)
	{
		WLD_PROFILE_FUNCTION();

		// 参数 0 表示默认行为。也可以使用 VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT，
		// 但通常我们希望保留预分配的内存块以供下一帧使用，减少内存抖动。
		VkCommandBufferResetFlags flags = releaseResources ? VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT : 0;
		if (vkResetCommandBuffer(m_CommandBuffers[index], flags) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to reset Vulkan Command Buffer at index {0}!", index);
		}
	}
}