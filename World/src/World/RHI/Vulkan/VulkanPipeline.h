#pragma once

#include "World/RHI/RhiPipeline.h"

#include <volk.h>

namespace World::Rhi::Vulkan
{
	class VulkanDevice;

	class VulkanPipeline final : public Pipeline
	{
	public:
		VulkanPipeline(VulkanDevice& device, const PipelineDesc& desc);
		~VulkanPipeline() override;
		const PipelineDesc& GetDesc() const override { return m_Desc; }
		VkPipeline GetPipeline() const { return m_Pipeline; }
		VkPipelineLayout GetLayout() const { return m_Layout; }
	private:
		VulkanDevice& m_Device;
		PipelineDesc m_Desc;
		VkPipeline m_Pipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_Layout = VK_NULL_HANDLE;
	};
}
