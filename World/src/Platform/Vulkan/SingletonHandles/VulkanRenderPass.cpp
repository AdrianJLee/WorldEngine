#include "wldpch.h"
#include "VulkanRenderPass.h"

namespace World
{
	static VkAttachmentLoadOp UtilsLoadOp(AttachmentLoadOp op)
	{
		switch (op)
		{
			case AttachmentLoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
			case AttachmentLoadOp::Load:  return VK_ATTACHMENT_LOAD_OP_LOAD;
		}
		return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	}

	VulkanRenderPass::VulkanRenderPass(const Ref<VulkanDevice>& device, const VulkanRenderPassSpecification& spec)
		: m_Device(device)
	{
		WLD_PROFILE_FUNCTION();
		std::vector<VkAttachmentDescription> attachments;
		std::vector<VkAttachmentReference> colorRefs;

		for (const auto& attachmentSpec : spec.Attachments)
		{
			// Attachment description
			VkAttachmentDescription desc {};
			desc.format = attachmentSpec.Format;
			// TODO: 这里可以考虑使用多重采样来进行抗锯齿，目前先使用单样本
			desc.samples = VK_SAMPLE_COUNT_1_BIT;
			desc.loadOp = UtilsLoadOp(attachmentSpec.LoadOp);
			desc.storeOp = (attachmentSpec.StoreOp == AttachmentStoreOp::Store) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
			desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			desc.initialLayout = attachmentSpec.InitialLayout;
			desc.finalLayout = attachmentSpec.FinalLayout;
			attachments.push_back(desc);


			// Attachment reference for the subpass
			VkAttachmentReference ref {};
			ref.attachment = (uint32_t)attachments.size() - 1;
			ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			colorRefs.push_back(ref);
		}

		// Subpass description
		VkSubpassDescription subpass {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = (uint32_t)colorRefs.size();
		subpass.pColorAttachments = colorRefs.data();

		// Subpass dependency to handle layout transitions
		VkSubpassDependency dependency {};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		// Create the render pass
		VkRenderPassCreateInfo renderPassInfo {};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = (uint32_t)attachments.size();
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;

		if (vkCreateRenderPass(m_Device->GetLogicalDevice(), &renderPassInfo, nullptr, &m_RenderPass) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to create render pass!");
		}

		WLD_CORE_INFO("Vulkan RenderPass created successfully.");
	}
	VulkanRenderPass::~VulkanRenderPass()
	{
		WLD_PROFILE_FUNCTION();

		if (m_RenderPass)
		{
			vkDestroyRenderPass(m_Device->GetLogicalDevice(), m_RenderPass, nullptr);
		}

	}



}

