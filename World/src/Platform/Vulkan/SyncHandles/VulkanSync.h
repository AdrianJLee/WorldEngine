#pragma once
#include "Platform/Vulkan/SingletonHandles/VulkanDevice.h"
namespace World
{
	class VulkanSync
	{
	public:
		VulkanSync(const Ref<VulkanDevice>& device, uint32_t maxFramesInFlight);
		~VulkanSync();

		VkSemaphore GetImageAvailableSemaphore(uint32_t frameIndex) const { return m_ImageAvailableSemaphores[frameIndex]; }
		VkSemaphore GetRenderFinishedSemaphore(uint32_t frameIndex) const { return m_RenderFinishedSemaphores[frameIndex]; }
		VkFence GetInFlightFence(uint32_t frameIndex) const { return m_InFlightFences[frameIndex]; }

		void WaitForFence(uint32_t frame);
		void ResetFence(uint32_t frame);
	private:
		Ref<VulkanDevice> m_Device;
		std::vector<VkSemaphore> m_ImageAvailableSemaphores;
		std::vector<VkSemaphore> m_RenderFinishedSemaphores;
		std::vector<VkFence> m_InFlightFences;
	};
}