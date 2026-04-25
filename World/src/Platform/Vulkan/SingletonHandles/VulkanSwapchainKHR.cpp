#include "wldpch.h"
#include "VulkanSwapchainKHR.h"

namespace World
{
	VulkanSwapchainKHR::VulkanSwapchainKHR(const Ref<VulkanDevice>& device, const Ref<VulkanSurfaceKHR>& surface, uint32_t width, uint32_t height)
		:m_Device(device), m_Surface(surface)
	{
		WLD_PROFILE_FUNCTION();

		VkPhysicalDevice physicalDeviceHandle = m_Device->GetVulkanPhysicalDevice()->GetPhysicalDevice();
		VkSurfaceKHR surfaceHandle = m_Surface->GetSurface();

		// Get the surface capabilities to determine the swap chain settings
		VkSurfaceCapabilitiesKHR capabilities;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDeviceHandle, surfaceHandle, &capabilities);

		// Query the supported surface formats and present modes
		uint32_t formatCount;
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDeviceHandle, surfaceHandle, &formatCount, nullptr);
		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDeviceHandle, surfaceHandle, &formatCount, formats.data());

		uint32_t presentModeCount;
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDeviceHandle, surfaceHandle, &presentModeCount, nullptr);
		std::vector<VkPresentModeKHR> presentModes(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDeviceHandle, surfaceHandle, &presentModeCount, presentModes.data());

		// Choose the best surface format, present mode, and swap extent based on the capabilities and preferences
		VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(formats);
		VkPresentModeKHR presentMode = ChooseSwapPresentMode(presentModes);
		m_SwapChainExtent = ChooseSwapExtent(capabilities, width, height);
		m_SwapChainImageFormat = surfaceFormat.format;

		// Determine the number of images in the swap chain (double buffering or triple buffering)
		uint32_t imageCount = capabilities.minImageCount + 1;
		if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
		{
			imageCount = capabilities.maxImageCount;
		}

		// Create the swap chain
		VkSwapchainCreateInfoKHR createInfo {};
		createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		createInfo.surface = surfaceHandle;
		createInfo.minImageCount = imageCount;
		createInfo.imageFormat = surfaceFormat.format;
		createInfo.imageColorSpace = surfaceFormat.colorSpace;
		createInfo.imageExtent = m_SwapChainExtent;
		createInfo.imageArrayLayers = 1;// If using stereoscopic 3D, this would be 2
		createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // We will render directly to the swap chain images

		// If the graphics and presentation queues are different, we need to specify how the images will be shared between them
		uint32_t queueFamilyIndices[] = {
			m_Device->GetVulkanPhysicalDevice()->GetQueueFamilyIndices().GraphicsFamily.value(),
			m_Device->GetVulkanPhysicalDevice()->GetQueueFamilyIndices().PresentFamily.value()
		};
		if (queueFamilyIndices[0] != queueFamilyIndices[1])
		{
			createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			createInfo.queueFamilyIndexCount = 2;
			createInfo.pQueueFamilyIndices = queueFamilyIndices;
		}
		else
		{
			createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}

		createInfo.preTransform = capabilities.currentTransform;
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		createInfo.presentMode = presentMode;
		createInfo.clipped = VK_TRUE;
		createInfo.oldSwapchain = VK_NULL_HANDLE;

		if (vkCreateSwapchainKHR(m_Device->GetLogicalDevice(), &createInfo, nullptr, &m_Swapchain) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to create swap chain!");
		}

		// Get the swap chain images
		vkGetSwapchainImagesKHR(m_Device->GetLogicalDevice(), m_Swapchain, &imageCount, nullptr);
		m_SwapChainImages.resize(imageCount);
		vkGetSwapchainImagesKHR(m_Device->GetLogicalDevice(), m_Swapchain, &imageCount, m_SwapChainImages.data());

		// Create image views for the swap chain images
		m_SwapChainImageViews.resize(imageCount);
		for (size_t i = 0; i < m_SwapChainImages.size(); i++)
		{
			VkImageViewCreateInfo viewInfo {};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = m_SwapChainImages[i];
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = m_SwapChainImageFormat;
			viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			viewInfo.subresourceRange.baseMipLevel = 0;
			viewInfo.subresourceRange.levelCount = 1;
			viewInfo.subresourceRange.baseArrayLayer = 0;
			viewInfo.subresourceRange.layerCount = 1;

			if (vkCreateImageView(m_Device->GetLogicalDevice(), &viewInfo, nullptr, &m_SwapChainImageViews[i]) != VK_SUCCESS)
			{
				WLD_CORE_ASSERT(false, "Failed to create image views!");
			}
		}

		WLD_CORE_INFO("Vulkan Swapchain created with {0} images.", imageCount);
	}

	VulkanSwapchainKHR::~VulkanSwapchainKHR()
	{
		WLD_PROFILE_FUNCTION();
		for (auto imageView : m_SwapChainImageViews)
		{
			vkDestroyImageView(m_Device->GetLogicalDevice(), imageView, nullptr);
		}

		if (m_Swapchain)
		{
			vkDestroySwapchainKHR(m_Device->GetLogicalDevice(), m_Swapchain, nullptr);
		}
	}
	VkSurfaceFormatKHR VulkanSwapchainKHR::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats)
	{
		WLD_PROFILE_FUNCTION();
		for (const auto& availableFormat : availableFormats)
		{
			if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
				availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				return availableFormat;
			}
		}

		return availableFormats[0];
	}
	VkPresentModeKHR VulkanSwapchainKHR::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes)
	{
		WLD_PROFILE_FUNCTION();
		// 最佳选择：MAILBOX (三重缓冲)
		// 它不会导致画面撕裂，且延迟远低于标准的 FIFO（双重缓冲垂直同步）
		// 对于高性能游戏引擎，这是压榨显卡性能的首选。
		for (const auto& availablePresentMode : availablePresentModes)
		{
			if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
			{
				WLD_CORE_INFO("Swapchain Present Mode: Mailbox (Triple Buffering)");
				return availablePresentMode;
			}
		}

		// 备选方案：IMMEDIATE (不等待垂直同步)
		// 可能会产生画面撕裂，但延迟最低。

		// 保底方案：FIFO (强制垂直同步)
		// Vulkan 标准要求必须支持此模式。
		WLD_CORE_INFO("Swapchain Present Mode: FIFO (Standard V-Sync)");
		return VK_PRESENT_MODE_FIFO_KHR;
	}
	VkExtent2D VulkanSwapchainKHR::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height)
	{
		WLD_PROFILE_FUNCTION();
		// 如果 currentExtent 的宽高不是 0xFFFFFFFF，说明驱动已经帮我们定好了大小
		if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
		{
			return capabilities.currentExtent;
		}
		else
		{
			// 否则我们需要手动根据窗口大小计算，并确保在硬件支持的最小值和最大值之间
			VkExtent2D actualExtent = { width, height };

			actualExtent.width = std::clamp(actualExtent.width,
				capabilities.minImageExtent.width,
				capabilities.maxImageExtent.width);
			actualExtent.height = std::clamp(actualExtent.height,
				capabilities.minImageExtent.height,
				capabilities.maxImageExtent.height);

			return actualExtent;
		}
	}
}