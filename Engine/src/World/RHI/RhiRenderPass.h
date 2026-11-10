#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	struct RenderPassAttachment
	{
		Format Format = Format::Undefined;
		SampleCount Samples = SampleCount::Count1;
		LoadOp Load = LoadOp::DontCare;
		StoreOp Store = StoreOp::DontCare;
		AttachmentLayout InitialLayout = AttachmentLayout::Undefined;
		AttachmentLayout FinalLayout = AttachmentLayout::Undefined;
		ClearValue Clear;
	};

	struct AttachmentRef
	{
		// UINT32_MAX 表示"未使用"。默认值必须是 UINT32_MAX:
		// SubpassDesc::HasDepthStencil() 依赖它判断是否真的绑定了深度附件,
		// 否则没有深度附件的通道(如 WUI 的呈现通道)会把颜色附件当成
		// 深度附件引用,导致 VkRenderPass 非法、渲染整体失效。
		uint32_t Index = UINT32_MAX;
		AttachmentLayout Layout = AttachmentLayout::Undefined;
	};

	struct SubpassDesc
	{
		std::vector<AttachmentRef> ColorAttachments;
		AttachmentRef DepthStencilAttachment;   // Index == UINT32_MAX 表示无
		// P4-4a:与 ColorAttachments **位置对应** —— 第 k 项 = 第 k 个颜色附件的 resolve 目标,
		// 值是**渲染通道级**附件索引;UINT32_MAX = 该附件不 resolve。深度不做 resolve。
		// 两个后端都要求:被 resolve 的颜色附件与 resolve 目标同格式(采样数不同)。
		std::vector<uint32_t> ResolveAttachments;
		std::vector<uint32_t> PreserveAttachments;
		std::vector<uint32_t> InputAttachments;
		bool HasDepthStencil() const { return DepthStencilAttachment.Index != UINT32_MAX; }
	};

	struct SubpassDependency
	{
		uint32_t SrcSubpass = UINT32_MAX;   // UINT32_MAX = 外部(VK_SUBPASS_EXTERNAL)
		uint32_t DstSubpass = 0;
		uint32_t SrcStages = PipelineStageNone;
		uint32_t DstStages = PipelineStageNone;
		uint32_t SrcAccess = AccessNone;
		uint32_t DstAccess = AccessNone;
		bool ByRegion = false;
	};

	struct RenderPassDesc
	{
		std::vector<RenderPassAttachment> Attachments;
		std::vector<SubpassDesc> Subpasses;
		std::vector<SubpassDependency> Dependencies;
		std::string DebugName;
	};

	class WLD_API RenderPass
	{
	public:
		virtual ~RenderPass() = default;
		virtual const RenderPassDesc& GetDesc() const = 0;
	};

	struct FramebufferDesc
	{
		Handle<RenderPass> RenderPass;
		Extent2D Extent;
		uint32_t Layers = 1;
		std::vector<Handle<Rhi::Texture>> Attachments;   // 顺序对应 RenderPassDesc::Attachments
		std::string DebugName;
	};

	class WLD_API Framebuffer
	{
	public:
		virtual ~Framebuffer() = default;
		virtual const FramebufferDesc& GetDesc() const = 0;
	};
}
