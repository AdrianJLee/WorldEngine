#include "wldpch.h"
#include "VulkanPipeline.h"

namespace World
{
	static VkFormat ShaderDataTypeToVulkanFormat(ShaderDataType type)
	{
		switch (type)
		{
			case ShaderDataType::Float:    return VK_FORMAT_R32_SFLOAT;
			case ShaderDataType::Float2:   return VK_FORMAT_R32G32_SFLOAT;
			case ShaderDataType::Float3:   return VK_FORMAT_R32G32B32_SFLOAT;
			case ShaderDataType::Float4:   return VK_FORMAT_R32G32B32A32_SFLOAT;
			case ShaderDataType::Int:      return VK_FORMAT_R32_SINT;
			case ShaderDataType::Int2:     return VK_FORMAT_R32G32_SINT;
			case ShaderDataType::Int3:     return VK_FORMAT_R32G32B32_SINT;
			case ShaderDataType::Int4:     return VK_FORMAT_R32G32B32A32_SINT;
			case ShaderDataType::Bool:     return VK_FORMAT_R8_UINT; // Vulkan usually uses 8-bit or 32-bit uint for bools depending on alignment
		}
		WLD_CORE_ASSERT(false, "Unknown ShaderDataType!");
		return VK_FORMAT_UNDEFINED;
	}

	VulkanPipeline::VulkanPipeline(const Ref<VulkanDevice>& device, const PipelineSpecification& spec)
		: m_Device(device), m_PipelineSpec(spec)
	{
		WLD_PROFILE_FUNCTION();
		CreateGraphicsPipeline();
	}
	VulkanPipeline::~VulkanPipeline()
	{
		WLD_PROFILE_FUNCTION();
		vkDestroyPipeline(m_Device->GetLogicalDevice(), m_GraphicsPipeline, nullptr);
		vkDestroyPipelineLayout(m_Device->GetLogicalDevice(), m_PipelineLayout, nullptr);
	}
	void VulkanPipeline::Bind(VkCommandBuffer commandBuffer)
	{
		WLD_PROFILE_FUNCTION();

		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GraphicsPipeline);
	}
	void VulkanPipeline::CreateGraphicsPipeline()
	{
		WLD_PROFILE_FUNCTION();
		// Create the vertex input state based on the provided BufferLayout
		VkVertexInputBindingDescription bindingDescription {};
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions;

		if (m_PipelineSpec.Layout.GetElements().size() > 0)
		{
			// Set binding description: 绑定点、每个顶点的字节大小、输入速率
			bindingDescription.binding = 0;
			bindingDescription.stride = m_PipelineSpec.Layout.GetStride();
			bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

			// Set attribute descriptions: 位置、绑定点、格式、偏移
			uint32_t location = 0;
			for (const auto& element : m_PipelineSpec.Layout)
			{
				VkVertexInputAttributeDescription attributeDesc {};
				attributeDesc.binding = 0;
				attributeDesc.location = element.Location;
				attributeDesc.format = ShaderDataTypeToVulkanFormat(element.Type);
				attributeDesc.offset = element.Offset;
				attributeDescriptions.push_back(attributeDesc);

				location++;
			}
		}

		// Create the vertex input state
		VkPipelineVertexInputStateCreateInfo vertexInputInfo { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		if (m_PipelineSpec.Layout.GetElements().size() > 0)
		{
			vertexInputInfo.vertexBindingDescriptionCount = 1;
			vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
			vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
			vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
		}
		else
		{
			vertexInputInfo.vertexBindingDescriptionCount = 0;
			vertexInputInfo.pVertexBindingDescriptions = nullptr;
			vertexInputInfo.vertexAttributeDescriptionCount = 0;
			vertexInputInfo.pVertexAttributeDescriptions = nullptr;
		}

		// Create the input assembly state
		VkPipelineInputAssemblyStateCreateInfo inputAssembly { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		inputAssembly.primitiveRestartEnable = VK_FALSE;

		// Create the rasterizer state
		VkPipelineRasterizationStateCreateInfo rasterizer { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		rasterizer.depthClampEnable = VK_FALSE;
		rasterizer.rasterizerDiscardEnable = VK_FALSE;
		rasterizer.polygonMode = m_PipelineSpec.Wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
		rasterizer.lineWidth = m_PipelineSpec.LineWidth;
		rasterizer.cullMode = m_PipelineSpec.BackfaceCulling ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
		rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

		// Create the multisampling state
		VkPipelineMultisampleStateCreateInfo multisampling { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		multisampling.sampleShadingEnable = VK_FALSE;
		//multisampling.rasterizationSamples = m_PipelineSpec.Samples;

		// Create the depth stencil state
		VkPipelineDepthStencilStateCreateInfo depthStencil { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		depthStencil.depthTestEnable = m_PipelineSpec.DepthTest ? VK_TRUE : VK_FALSE;
		depthStencil.depthWriteEnable = m_PipelineSpec.DepthWrite ? VK_TRUE : VK_FALSE;
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
		depthStencil.depthBoundsTestEnable = VK_FALSE;
		depthStencil.stencilTestEnable = VK_FALSE;

		// Create the color blend attachment state
		VkPipelineColorBlendAttachmentState colorBlendAttachment {};
		colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		colorBlendAttachment.blendEnable = VK_TRUE;
		colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

		// Create the color blend state
		VkPipelineColorBlendStateCreateInfo colorBlending { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		colorBlending.logicOpEnable = VK_FALSE;
		colorBlending.attachmentCount = 1;
		colorBlending.pAttachments = &colorBlendAttachment;

		// Create the pipeline layout (Descriptor Set Layouts and Push Constants)
		VkPipelineLayoutCreateInfo pipelineLayoutInfo { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		pipelineLayoutInfo.setLayoutCount = 0; // TODO: 添加 Descriptor Set Layout (例如 Uniform Buffers, Textures)
		pipelineLayoutInfo.pushConstantRangeCount = 0; // TODO: 添加 Push Constants

		// Create Pipeline Layout
		if (vkCreatePipelineLayout(m_Device->GetLogicalDevice(), &pipelineLayoutInfo, nullptr, &m_PipelineLayout) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to create pipeline layout!");
		}

		// Create Dynamic State (例如 Viewport 和 Scissor 可以动态设置)
		std::vector<VkDynamicState> dynamicStates = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR
		};
		VkPipelineDynamicStateCreateInfo dynamicState { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
		dynamicState.pDynamicStates = dynamicStates.data();

		VkPipelineViewportStateCreateInfo viewportState { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		viewportState.viewportCount = 1;
		viewportState.pViewports = nullptr; // 动态的可以在 Bind 后指定
		viewportState.scissorCount = 1;
		viewportState.pScissors = nullptr;  // 动态的可以在 Bind 后指定


		// Create the graphics pipeline
		VkGraphicsPipelineCreateInfo pipelineInfo { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		//const auto& shaderStages = m_PipelineSpec.Shader->GetShaderStages();
		//pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
		//pipelineInfo.pStages = shaderStages.data();

		pipelineInfo.pVertexInputState = &vertexInputInfo;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisampling;
		pipelineInfo.pDepthStencilState = &depthStencil;
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDynamicState = &dynamicState;

		pipelineInfo.layout = m_PipelineLayout;
		//pipelineInfo.renderPass = m_PipelineSpec.RenderPass->GetRenderPass();
		pipelineInfo.subpass = 0;

		if (vkCreateGraphicsPipelines(m_Device->GetLogicalDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_GraphicsPipeline) != VK_SUCCESS)
		{
			WLD_CORE_ASSERT(false, "Failed to create graphics pipeline!");
		}

		WLD_CORE_INFO("Vulkan Pipeline '{0}' created.", m_PipelineSpec.DebugName);
	}
}