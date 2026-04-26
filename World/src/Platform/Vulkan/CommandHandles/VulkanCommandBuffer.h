#pragma once
#include "Platform/Vulkan/SingletonHandles/VulkanDevice.h"
#include "Platform/Vulkan/CommandHandles/VulkanCommandPool.h"
namespace World
{
	class VulkanCommandBuffer
	{
	public:
		VulkanCommandBuffer(Ref<VulkanDevice> device, Ref<VulkanCommandPool> commandPool, uint32_t count);
		~VulkanCommandBuffer();


		void Begin(uint32_t index);
		void End(uint32_t index);

		void Reset(uint32_t index, bool releaseResources = false);

		VkCommandBuffer GetCommandBuffer(uint32_t index) const { return m_CommandBuffers[index]; }
	private:
		std::vector<VkCommandBuffer> m_CommandBuffers;

		Ref<VulkanDevice> m_Device;
		Ref<VulkanCommandPool> m_CommandPool;

	};
}