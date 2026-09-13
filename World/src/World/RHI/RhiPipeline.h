#pragma once

#include "World/RHI/RhiCore.h"
#include "World/RHI/RhiShader.h"

namespace World::Rhi
{
	class RenderPass;

	struct VertexAttribute
	{
		uint32_t Location = 0;
		uint32_t Binding = 0;
		Format Format = Format::Undefined;
		uint32_t Offset = 0;
	};

	struct VertexBinding
	{
		uint32_t Binding = 0;
		uint32_t Stride = 0;
		bool PerInstance = false;
	};

	struct PushConstantRange
	{
		ShaderStageFlags Stages = 0;
		uint32_t Offset = 0;
		uint32_t Size = 0;
	};

	struct StencilState
	{
		CompareOp Compare = CompareOp::Always;
		StencilOp Fail = StencilOp::Keep;
		StencilOp DepthFail = StencilOp::Keep;
		StencilOp Pass = StencilOp::Keep;
		uint32_t Reference = 0;
		uint32_t CompareMask = 0xFF;
		uint32_t WriteMask = 0xFF;
	};

	struct DepthStencilState
	{
		bool DepthTest = true;
		bool DepthWrite = true;
		CompareOp DepthCompare = CompareOp::LessOrEqual;
		bool StencilTest = false;
		StencilState Front;
		StencilState Back;
	};

	struct BlendAttachmentState
	{
		bool BlendEnable = false;
		BlendFactor SrcColor = BlendFactor::One;
		BlendFactor DstColor = BlendFactor::Zero;
		BlendOp ColorOp = BlendOp::Add;
		BlendFactor SrcAlpha = BlendFactor::One;
		BlendFactor DstAlpha = BlendFactor::Zero;
		BlendOp AlphaOp = BlendOp::Add;
		uint8_t ColorWriteMask = 0xF;   // RGBA
	};

	struct PipelineDesc
	{
		Handle<Shader> Shader;
		Handle<RenderPass> RenderPass;      // 兼容渲染通道
		uint32_t SubpassIndex = 0;
		std::vector<VertexBinding> VertexBindings;
		std::vector<VertexAttribute> VertexAttributes;
		PrimitiveTopology Topology = PrimitiveTopology::TriangleList;
		PolygonMode Polygon = PolygonMode::Fill;
		CullMode Cull = CullMode::Back;
		FrontFace Front = FrontFace::CounterClockwise;
		DepthStencilState DepthStencil;
		std::vector<BlendAttachmentState> Blends;   // 每颜色附件一个;空=默认关闭混合
		SampleCount Samples = SampleCount::Count1;
		float LineWidth = 1.0f;
		bool PrimitiveRestart = false;
		bool RasterizerDiscard = false;
		bool DepthClamp = false;
		bool AlphaToCoverage = false;
		uint32_t SampleMask = 0xFFFFFFFF;
		float DepthBiasConstant = 0.0f;
		float DepthBiasSlope = 0.0f;
		float DepthBiasClamp = 0.0f;
		std::vector<PushConstantRange> PushConstants;
		std::string DebugName;
	};

	class WLD_API Pipeline
	{
	public:
		virtual ~Pipeline() = default;
		virtual const PipelineDesc& GetDesc() const = 0;
	};
}
