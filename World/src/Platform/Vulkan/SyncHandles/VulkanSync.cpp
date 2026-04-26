#include "wldpch.h"
#include "VulkanSync.h"

namespace World
{
	VulkanSync::VulkanSync(const Ref<VulkanDevice>& device, uint32_t maxFramesInFlight)
		: m_Device(device)
	{
		WLD_PROFILE_FUNCTION();

		m_ImageAvailableSemaphores.resize(maxFramesInFlight);
		m_RenderFinishedSemaphores.resize(maxFramesInFlight);
		m_InFlightFences.resize(maxFramesInFlight);

		VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

		VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		for (uint32_t i = 0; i < maxFramesInFlight; i++)
		{
			vkCreateSemaphore(m_Device->GetLogicalDevice(), &semaphoreInfo, nullptr, &m_ImageAvailableSemaphores[i]);
			vkCreateSemaphore(m_Device->GetLogicalDevice(), &semaphoreInfo, nullptr, &m_RenderFinishedSemaphores[i]);
			vkCreateFence(m_Device->GetLogicalDevice(), &fenceInfo, nullptr, &m_InFlightFences[i]);
		}
	}
	VulkanSync::~VulkanSync()
	{
		WLD_PROFILE_FUNCTION();

		for (size_t i = 0; i < m_InFlightFences.size(); i++)
		{
			vkDestroySemaphore(m_Device->GetLogicalDevice(), m_ImageAvailableSemaphores[i], nullptr);
			vkDestroySemaphore(m_Device->GetLogicalDevice(), m_RenderFinishedSemaphores[i], nullptr);
			vkDestroyFence(m_Device->GetLogicalDevice(), m_InFlightFences[i], nullptr);
		}
	}
	void VulkanSync::WaitForFence(uint32_t frame)
	{
		vkWaitForFences(m_Device->GetLogicalDevice(), 1, &m_InFlightFences[frame], VK_TRUE, UINT64_MAX);
	}
	void VulkanSync::ResetFence(uint32_t frame)
	{
		vkResetFences(m_Device->GetLogicalDevice(), 1, &m_InFlightFences[frame]);
	}
}