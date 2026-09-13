#pragma once

#include "World/RHI/RhiSwapchain.h"
#include "World/RHI/Vulkan/VulkanResources.h"

#include <volk.h>

namespace World::Rhi::Vulkan
{
	class VulkanDevice;

	class VulkanSwapchain final : public Swapchain
	{
	public:
		VulkanSwapchain(VulkanDevice& device, const SwapchainDesc& desc);
		~VulkanSwapchain() override;
		AcquireResult AcquireNext(const Handle<Semaphore>& signalWhenReady = nullptr) override;
		void Present(const Handle<Semaphore>& waitBeforePresent = nullptr) override;
		Extent2D GetExtent() const override;
		void Resize(Extent2D extent) override;
		VkSwapchainKHR GetSwapchain() const { return m_Swapchain; }
		uint32_t GetImageCount() const { return static_cast<uint32_t>(m_Images.size()); }
		void TransitionImage(uint32_t index, VkImageLayout layout);
	private:
		VulkanDevice& m_Device;
		SwapchainDesc m_Desc;
		VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
		VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
		std::vector<VkImage> m_Images;
		std::vector<Handle<VulkanTexture>> m_ImageTextures;
		Extent2D m_Extent;
	};
}
