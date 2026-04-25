#pragma once
#include "VulkanDevice.h"
#include "VulkanSurfaceKHR.h"

#include <vulkan/vulkan.h>

namespace World
{
	class VulkanSwapchainKHR
	{
	public:
		VulkanSwapchainKHR(const Ref<VulkanDevice>& device, const Ref<VulkanSurfaceKHR>& surface, uint32_t width, uint32_t height);
		~VulkanSwapchainKHR();

		VulkanSwapchainKHR(const VulkanSwapchainKHR&) = delete;
		VulkanSwapchainKHR& operator=(const VulkanSwapchainKHR&) = delete;
	public:
		VkSwapchainKHR GetSwapchain() const { return m_Swapchain; }
		VkFormat GetSwapChainImageFormat() const { return m_SwapChainImageFormat; }
		VkExtent2D GetSwapChainExtent() const { return m_SwapChainExtent; }
		const std::vector<VkImageView>& GetSwapChainImageViews() const { return m_SwapChainImageViews; }

	private:
		// Helper functions to choose the best settings for the swap chain
		VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
		// Helper function to choose the best presentation mode for the swap chain
		VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
		// Helper function to choose the swap extent (resolution of the swap chain images)
		VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height);
	private:
		VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
		VkFormat m_SwapChainImageFormat;
		VkExtent2D m_SwapChainExtent;

		std::vector<VkImage> m_SwapChainImages;
		std::vector<VkImageView> m_SwapChainImageViews;

		Ref<VulkanDevice> m_Device;
		Ref<VulkanSurfaceKHR> m_Surface;
	};
}