#pragma once
#include "VulkanDevice.h"
#include <vulkan/vulkan.h>

namespace World
{
	enum class AttachmentLoadOp { Clear, Load, DontCare };
	enum class AttachmentStoreOp { Store, DontCare };

	struct RenderPassAttachment
	{
		VkFormat Format;
		AttachmentLoadOp LoadOp = AttachmentLoadOp::Clear;
		AttachmentStoreOp StoreOp = AttachmentStoreOp::Store;
		VkImageLayout InitialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImageLayout FinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	};

	struct VulkanRenderPassSpecification
	{
		std::vector<RenderPassAttachment> Attachments;
	};

	class VulkanRenderPass
	{
	public:
		VulkanRenderPass(const Ref<VulkanDevice>& device, const VulkanRenderPassSpecification& spec);
		~VulkanRenderPass();

		VulkanRenderPass(const VulkanRenderPass&) = delete;
		VulkanRenderPass& operator=(const VulkanRenderPass&) = delete;
	public:
		VkRenderPass GetRenderPass() const { return m_RenderPass; }
	private:
		VkRenderPass m_RenderPass = VK_NULL_HANDLE;
		VulkanRenderPassSpecification m_Specification;

		Ref<VulkanDevice> m_Device;
	};
}