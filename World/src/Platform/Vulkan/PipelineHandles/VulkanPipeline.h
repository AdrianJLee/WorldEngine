#pragma once
#include "Platform/Vulkan/SingletonHandles/VulkanRenderPass.h"
#include "World/Renderer/Buffer.h"
#include "Platform/Vulkan/DataHandles/VulkanShader.h"

#include <vulkan/vulkan.h>
namespace World
{
	struct PipelineSpecification
	{
		Ref<VulkanRenderPass> RenderPass;
		Ref<VulkanShader> Shader;

		BufferLayout Layout;

		bool BackfaceCulling = true;
		bool DepthTest = true;
		bool DepthWrite = true;
		bool Wireframe = false;
		float LineWidth = 1.0f;

		// 多重采样 (MSAA)
		VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT;

		std::string DebugName;
	};

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