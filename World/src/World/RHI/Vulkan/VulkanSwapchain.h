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
		uint32_t GetImageCount() const override { return static_cast<uint32_t>(m_Images.size()); }
		// signalAfter:非空时该信号量在转换提交完成时被发出(呈现前的转换用它排序);
		// waitBefore:非空时该转换命令等到 acquire 信号量后再执行。
		bool TransitionImage(uint32_t index, VkImageLayout layout,
			const Handle<Semaphore>& signalAfter = nullptr, const Handle<Semaphore>& waitBefore = nullptr);
	private:
		VulkanDevice& m_Device;
		SwapchainDesc m_Desc;
		VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
		VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
		uint32_t m_CurrentImageIndex = 0;
		std::vector<VkImage> m_Images;
		std::vector<Handle<VulkanTexture>> m_ImageTextures;
		Extent2D m_Extent;
	};
}
