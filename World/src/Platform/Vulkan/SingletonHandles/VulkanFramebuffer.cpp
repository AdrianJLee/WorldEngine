#include "wldpch.h"
#include "VulkanFramebuffer.h"

namespace World
{
	VulkanFramebuffer::VulkanFramebuffer(Ref<VulkanDevice> device, Ref<VulkanRenderPass> renderPass, Ref<VulkanSwapchainKHR> swapchain)
		: m_Device(device)
	{
		WLD_PROFILE_FUNCTION();

		const auto& swapChainImageViews = swapchain->GetSwapChainImageViews();
		const auto& swapChainExtent = swapchain->GetSwapChainExtent();

		m_Framebuffers.resize(swapChainImageViews.size());

		for (size_t i = 0; i < swapChainImageViews.size(); i++)
		{
			VkImageView attachments[] =
			{
				swapChainImageViews[i]

				// TODO:Depth attachment can be added here if needed in the future
			};

			VkFramebufferCreateInfo framebufferInfo {};
			framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferInfo.attachmentCount = static_cast<uint32_t>(std::size(attachments));
			framebufferInfo.pAttachments = attachments;
			framebufferInfo.renderPass = renderPass->GetRenderPass();
			framebufferInfo.width = swapChainExtent.width;
			framebufferInfo.height = swapChainExtent.height;

			if (vkCreateFramebuffer(m_Device->GetLogicalDevice(), &framebufferInfo, nullptr, &m_Framebuffers[i]) != VK_SUCCESS)
			{
				WLD_CORE_ASSERT(false, "Failed to create framebuffer!");
			}

			WLD_CORE_INFO("Vulkan Framebuffers created. Count: {0}", m_Framebuffers.size());
		}

	}
	VulkanFramebuffer::~VulkanFramebuffer()
	{
		WLD_PROFILE_FUNCTION();

		for (auto framebuffer : m_Framebuffers)
		{
			vkDestroyFramebuffer(m_Device->GetLogicalDevice(), framebuffer, nullptr);
		}
	}
}