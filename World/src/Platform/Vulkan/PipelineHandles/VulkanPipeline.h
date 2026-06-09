#pragma once
#include "Platform/Vulkan/SingletonHandles/VulkanRenderPass.h"
#include "World/Renderer/PipelineStateObject.h"
#include "World/Renderer/Buffer.h"
#include "Platform/Vulkan/DataHandles/VulkanShader.h"

#include <vulkan/vulkan.h>
namespace World
{

	class VulkanPipeline
	{
	public:
		VulkanPipeline(const Ref<VulkanDevice>& device, const PipelineSpecification& spec);
		~VulkanPipeline();

		VulkanPipeline(const VulkanPipeline&) = delete;
		VulkanPipeline& operator=(const VulkanPipeline&) = delete;

		void Bind(VkCommandBuffer commandBuffer);
		VkPipeline GetPipeline() const { return m_GraphicsPipeline; }
		VkPipelineLayout GetPipelineLayout() const { return m_PipelineLayout; }

	private:
		void CreateGraphicsPipeline();

	private:
		VkPipeline m_GraphicsPipeline = VK_NULL_HANDLE;
		VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;

		PipelineSpecification m_PipelineSpec;
		Ref<VulkanDevice> m_Device;
	};
}