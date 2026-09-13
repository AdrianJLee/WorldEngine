#include "wldpch.h"
#include "World/RHI/Vulkan/VulkanPipeline.h"
#include "World/RHI/Vulkan/VulkanDevice.h"
#include "World/RHI/Vulkan/VulkanResources.h"

#include <vector>

namespace World::Rhi::Vulkan
{
	namespace
	{
		VkFormat VertexFormat(Format format)
		{
			switch (format)
			{
				case Format::R32_SFLOAT: return VK_FORMAT_R32_SFLOAT;
				case Format::R32G32_SFLOAT: return VK_FORMAT_R32G32_SFLOAT;
				case Format::R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
				case Format::R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
				case Format::R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
				case Format::R32_SINT: return VK_FORMAT_R32_SINT;
				case Format::R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
				default: return VK_FORMAT_R32G32B32A32_SFLOAT;
			}
		}

		VkPrimitiveTopology Topology(PrimitiveTopology topology)
		{
			switch (topology)
			{
				case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
				case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
				case PrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
				case PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
				default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			}
		}
	}

	VulkanPipeline::VulkanPipeline(VulkanDevice& device, const PipelineDesc& desc)
		: m_Device(device), m_Desc(desc)
	{
		std::vector<VkPipelineShaderStageCreateInfo> stages;
		{
			const auto shader = std::dynamic_pointer_cast<VulkanShader>(desc.Shader);
			const std::vector<VkShaderModule>& modules = shader->GetModules();
			const ShaderDesc& shaderDesc = shader->GetDesc();
			size_t moduleIndex = 0;
			for (const ShaderStageSource& stage : shaderDesc.Stages)
			{
				if (stage.SpirV.empty() || moduleIndex >= modules.size())
					continue;
				VkPipelineShaderStageCreateInfo info{};
				info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				info.stage = stage.Stage == ShaderStage::Vertex ? VK_SHADER_STAGE_VERTEX_BIT
					: stage.Stage == ShaderStage::Fragment ? VK_SHADER_STAGE_FRAGMENT_BIT
					: stage.Stage == ShaderStage::Geometry ? VK_SHADER_STAGE_GEOMETRY_BIT
					: VK_SHADER_STAGE_COMPUTE_BIT;
				info.module = modules[moduleIndex++];
				info.pName = stage.EntryPoint.empty() ? "main" : stage.EntryPoint.c_str();
				stages.push_back(info);
			}
		}

		std::vector<VkDescriptorSetLayout> setLayouts;
		for (const Handle<DescriptorSetLayout>& layout : desc.DescriptorSetLayouts)
			if (const auto vulkanLayout = std::dynamic_pointer_cast<VulkanDescriptorSetLayout>(layout))
				setLayouts.push_back(vulkanLayout->GetLayout());
		VkPipelineLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
		layoutInfo.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
		vkCreatePipelineLayout(device.GetNativeDevice(), &layoutInfo, nullptr, &m_Layout);

		std::vector<VkVertexInputBindingDescription> bindings;
		for (const VertexBinding& binding : desc.VertexBindings)
			bindings.push_back({ binding.Binding, binding.Stride,
				binding.PerInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX });
		std::vector<VkVertexInputAttributeDescription> attributes;
		for (const VertexAttribute& attribute : desc.VertexAttributes)
			attributes.push_back({ attribute.Location, attribute.Binding,
				VertexFormat(attribute.Format), attribute.Offset });
		VkPipelineVertexInputStateCreateInfo vertexInput{};
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
		vertexInput.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
		vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
		vertexInput.pVertexAttributeDescriptions = attributes.empty() ? nullptr : attributes.data();

		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = Topology(desc.Topology);
		inputAssembly.primitiveRestartEnable = desc.PrimitiveRestart ? VK_TRUE : VK_FALSE;

		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo rasterization{};
		rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterization.polygonMode = desc.Polygon == PolygonMode::Line ? VK_POLYGON_MODE_LINE
			: desc.Polygon == PolygonMode::Point ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
		rasterization.cullMode = desc.Cull == CullMode::None ? VK_CULL_MODE_NONE
			: desc.Cull == CullMode::Front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
		rasterization.frontFace = desc.Front == FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterization.lineWidth = desc.LineWidth;
		rasterization.depthClampEnable = desc.DepthClamp ? VK_TRUE : VK_FALSE;
		rasterization.rasterizerDiscardEnable = desc.RasterizerDiscard ? VK_TRUE : VK_FALSE;
		rasterization.depthBiasEnable = (desc.DepthBiasConstant != 0.0f || desc.DepthBiasSlope != 0.0f) ? VK_TRUE : VK_FALSE;
		rasterization.depthBiasConstantFactor = desc.DepthBiasConstant;
		rasterization.depthBiasSlopeFactor = desc.DepthBiasSlope;
		rasterization.depthBiasClamp = desc.DepthBiasClamp;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = static_cast<VkSampleCountFlagBits>(static_cast<uint32_t>(desc.Samples));
		multisample.alphaToCoverageEnable = desc.AlphaToCoverage ? VK_TRUE : VK_FALSE;
		multisample.sampleShadingEnable = VK_FALSE;

		VkPipelineDepthStencilStateCreateInfo depthStencil{};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencil.depthTestEnable = desc.DepthStencil.DepthTest ? VK_TRUE : VK_FALSE;
		depthStencil.depthWriteEnable = desc.DepthStencil.DepthWrite ? VK_TRUE : VK_FALSE;
		depthStencil.depthCompareOp = static_cast<VkCompareOp>(static_cast<uint8_t>(desc.DepthStencil.DepthCompare));
		depthStencil.stencilTestEnable = desc.DepthStencil.StencilTest ? VK_TRUE : VK_FALSE;

		std::vector<VkPipelineColorBlendAttachmentState> blends;
		for (const BlendAttachmentState& blend : desc.Blends)
		{
			VkPipelineColorBlendAttachmentState out{};
			out.blendEnable = blend.BlendEnable ? VK_TRUE : VK_FALSE;
			out.srcColorBlendFactor = static_cast<VkBlendFactor>(static_cast<uint8_t>(blend.SrcColor));
			out.dstColorBlendFactor = static_cast<VkBlendFactor>(static_cast<uint8_t>(blend.DstColor));
			out.colorBlendOp = static_cast<VkBlendOp>(static_cast<uint8_t>(blend.ColorOp));
			out.srcAlphaBlendFactor = static_cast<VkBlendFactor>(static_cast<uint8_t>(blend.SrcAlpha));
			out.dstAlphaBlendFactor = static_cast<VkBlendFactor>(static_cast<uint8_t>(blend.DstAlpha));
			out.alphaBlendOp = static_cast<VkBlendOp>(static_cast<uint8_t>(blend.AlphaOp));
			out.colorWriteMask = blend.ColorWriteMask;
			blends.push_back(out);
		}
		VkPipelineColorBlendStateCreateInfo colorBlend{};
		colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlend.attachmentCount = static_cast<uint32_t>(blends.size());
		colorBlend.pAttachments = blends.empty() ? nullptr : blends.data();

		const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic{};
		dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates = dynamicStates;

		VkGraphicsPipelineCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		info.stageCount = static_cast<uint32_t>(stages.size());
		info.pStages = stages.data();
		info.pVertexInputState = &vertexInput;
		info.pInputAssemblyState = &inputAssembly;
		info.pViewportState = &viewportState;
		info.pRasterizationState = &rasterization;
		info.pMultisampleState = &multisample;
		info.pDepthStencilState = &depthStencil;
		info.pColorBlendState = &colorBlend;
		info.pDynamicState = &dynamic;
		info.layout = m_Layout;
		info.renderPass = desc.RenderPass
			? std::static_pointer_cast<VulkanRenderPass>(desc.RenderPass)->GetRenderPass()
			: VK_NULL_HANDLE;
		info.subpass = desc.SubpassIndex;
		vkCreateGraphicsPipelines(device.GetNativeDevice(), VK_NULL_HANDLE, 1, &info, nullptr, &m_Pipeline);
	}

	VulkanPipeline::~VulkanPipeline()
	{
		if (m_Pipeline) vkDestroyPipeline(m_Device.GetNativeDevice(), m_Pipeline, nullptr);
		if (m_Layout) vkDestroyPipelineLayout(m_Device.GetNativeDevice(), m_Layout, nullptr);
	}
}
