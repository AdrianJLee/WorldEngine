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
		uint32_t Index = 0;
		AttachmentLayout Layout = AttachmentLayout::Undefined;
	};

	struct SubpassDesc
	{
		std::vector<AttachmentRef> ColorAttachments;
		AttachmentRef DepthStencilAttachment;   // Index == UINT32_MAX 表示无
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
		std::vector<Handle<Texture>> Attachments;   // 顺序对应 RenderPassDesc::Attachments
		std::string DebugName;
	};

	class WLD_API Framebuffer
	{
	public:
		virtual ~Framebuffer() = default;
		virtual const FramebufferDesc& GetDesc() const = 0;
	};
}
