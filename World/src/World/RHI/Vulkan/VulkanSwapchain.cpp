#define VK_USE_PLATFORM_WIN32_KHR
#include "wldpch.h"
#include "World/RHI/Vulkan/VulkanSwapchain.h"
#include "World/RHI/Vulkan/VulkanDevice.h"
#include "World/RHI/Vulkan/VulkanResources.h"
#include "World/Core/Log.h"

// 本 TU 需要 volk 的 Win32 平台头(vulkan_win32.h 来自 Vulkan SDK include 目录),
// 但 GLFW_INCLUDE_VULKAN 是 World 目标的全局编译宏,会让 glfw3.h 去包含 vendor
// 精简版 vulkan.h(其中没有平台头)。这里在引入 GLFW 前取消该宏。
#undef GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

namespace World::Rhi::Vulkan
{
	VulkanSwapchain::VulkanSwapchain(VulkanDevice& device, const SwapchainDesc& desc)
		: m_Device(device), m_Desc(desc)
	{
		// 窗口在宿主内固定带 OpenGL 上下文(双后端共用一个 HWND),GLFW 的
		// glfwCreateWindowSurface 会因 client != GLFW_NO_API 拒绝;直接用
		// Win32 surface,拿到 HWND 后交给 Vulkan。
		const HWND hwnd = glfwGetWin32Window(static_cast<GLFWwindow*>(desc.NativeWindow));
		if (!hwnd)
		{
			if (Log::GetCoreLogger())
				WLD_CORE_ERROR("[RHI-VK] swapchain surface creation failed: no native window handle");
			return;
		}
		VkWin32SurfaceCreateInfoKHR surfaceInfo {};
		surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		surfaceInfo.hinstance = GetModuleHandleW(nullptr);
		surfaceInfo.hwnd = hwnd;
		if (vkCreateWin32SurfaceKHR(device.GetInstance(), &surfaceInfo, nullptr, &m_Surface) != VK_SUCCESS)
		{
			if (Log::GetCoreLogger())
				WLD_CORE_ERROR("[RHI-VK] vkCreateWin32SurfaceKHR failed");
			return;
		}

		VkSurfaceCapabilitiesKHR capabilities{};
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.GetPhysicalDevice(), m_Surface, &capabilities);
		m_Extent = { capabilities.currentExtent.width, capabilities.currentExtent.height };

		VkSwapchainCreateInfoKHR info{};
		info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		info.surface = m_Surface;
		info.minImageCount = desc.ImageCount ? desc.ImageCount : 3;
		info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
		info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		info.imageExtent = { m_Extent.Width, m_Extent.Height };
		info.imageArrayLayers = 1;
		info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.preTransform = capabilities.currentTransform;
		info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		info.presentMode = desc.Present == PresentMode::Immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
			: desc.Present == PresentMode::Mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
		info.clipped = VK_TRUE;
		vkCreateSwapchainKHR(device.GetNativeDevice(), &info, nullptr, &m_Swapchain);

		uint32_t count = 0;
		vkGetSwapchainImagesKHR(device.GetNativeDevice(), m_Swapchain, &count, nullptr);
		m_Images.resize(count);
		vkGetSwapchainImagesKHR(device.GetNativeDevice(), m_Swapchain, &count, m_Images.data());

		TextureDesc imageDesc;
		imageDesc.Type = TextureType::Texture2D;
		imageDesc.Format = Format::B8G8R8A8_UNORM;
		imageDesc.Extent = { m_Extent.Width, m_Extent.Height, 1 };
		imageDesc.Usage = TextureUsageColorAttachment | TextureUsageSampled;
		m_ImageTextures.clear();
		m_ImageTextures.reserve(m_Images.size());
		for (VkImage image : m_Images)
			m_ImageTextures.push_back(CreateRef<VulkanTexture>(device, imageDesc, image));
	}

	VulkanSwapchain::~VulkanSwapchain()
	{
		if (m_Swapchain) vkDestroySwapchainKHR(m_Device.GetNativeDevice(), m_Swapchain, nullptr);
		if (m_Surface) vkDestroySurfaceKHR(m_Device.GetInstance(), m_Surface, nullptr);
	}

	AcquireResult VulkanSwapchain::AcquireNext(const Handle<Semaphore>& signalWhenReady)
	{
		AcquireResult result;
		VkSemaphore semaphore = VK_NULL_HANDLE;
		if (signalWhenReady)
			semaphore = std::static_pointer_cast<VulkanSemaphore>(signalWhenReady)->GetSemaphore();
		const VkResult status = vkAcquireNextImageKHR(m_Device.GetNativeDevice(), m_Swapchain,
			UINT64_MAX, semaphore, VK_NULL_HANDLE, &result.ImageIndex);
		if (status == VK_ERROR_OUT_OF_DATE_KHR)
		{
			result.OutOfDate = true;
			return result;
		}
		if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
			return result;
		m_CurrentImageIndex = result.ImageIndex;
		if (result.ImageIndex < m_ImageTextures.size())
			result.Image = m_ImageTextures[result.ImageIndex];
		return result;
	}

	void VulkanSwapchain::Present(const Handle<Semaphore>& waitBeforePresent)
	{
		VkSemaphore wait = VK_NULL_HANDLE;
		if (waitBeforePresent)
			wait = std::static_pointer_cast<VulkanSemaphore>(waitBeforePresent)->GetSemaphore();
		VkPresentInfoKHR info{};
		info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		info.waitSemaphoreCount = wait ? 1u : 0u;
		info.pWaitSemaphores = wait ? &wait : nullptr;
		info.swapchainCount = 1;
		info.pSwapchains = &m_Swapchain;
		info.pImageIndices = &m_CurrentImageIndex;
		vkQueuePresentKHR(m_Device.GetGraphicsQueue(), &info);
	}

	Extent2D VulkanSwapchain::GetExtent() const { return m_Extent; }
	void VulkanSwapchain::Resize(Extent2D) { /* 重建由宿主触发;此处保持现状 */ }

	bool VulkanSwapchain::TransitionImage(uint32_t index, VkImageLayout layout,
		const Handle<Semaphore>& signalAfter, const Handle<Semaphore>& waitBefore)
	{
		if (index >= m_ImageTextures.size() || !m_ImageTextures[index])
			return false;
		// B0/B2:交换链图像布局转换是每帧两次的常驻操作,提交后不等待——
		// 转换与后续渲染/呈现同队列,队列顺序保证可见性(旧实现在这里整队列排空两次)。
		const auto texture = std::static_pointer_cast<VulkanTexture>(m_ImageTextures[index]);
		const VkImageLayout oldLayout = texture->GetLayout();
		VkSemaphore signal = VK_NULL_HANDLE;
		if (signalAfter)
			signal = std::static_pointer_cast<VulkanSemaphore>(signalAfter)->GetSemaphore();
		VkSemaphore wait = VK_NULL_HANDLE;
		if (waitBefore)
			wait = std::static_pointer_cast<VulkanSemaphore>(waitBefore)->GetSemaphore();
		const bool needsBarrier = oldLayout != layout;
		if (!needsBarrier && signal == VK_NULL_HANDLE && wait == VK_NULL_HANDLE)
			return false;
		const bool submitted = m_Device.SubmitOneShot([&](VkCommandBuffer commandBuffer)
		{
			if (!needsBarrier)
				return;   // 布局已就位:这里只需要发出/消费信号量
			VkImageMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = oldLayout;
			barrier.newLayout = layout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = texture->GetImage();
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.layerCount = 1;
			barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.dstAccessMask = (layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
				? 0 : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			vkCmdPipelineBarrier(commandBuffer,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				(layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
					? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier);
		}, false, wait, signal);
		if (needsBarrier && submitted)
			texture->SetLayout(layout);
		// 返回 true 表示"调用方可以等待 signalAfter"。
		return submitted && signal != VK_NULL_HANDLE;
	}
}
