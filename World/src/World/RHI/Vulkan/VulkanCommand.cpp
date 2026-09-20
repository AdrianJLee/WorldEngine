#include "wldpch.h"
#include "World/RHI/Vulkan/VulkanCommand.h"
#include "World/RHI/Vulkan/VulkanDevice.h"
#include "World/RHI/Vulkan/VulkanPipeline.h"
#include "World/RHI/Vulkan/VulkanResources.h"
#include "World/Core/Log.h"

#include <vector>

namespace World::Rhi::Vulkan
{
	namespace
	{
		VkIndexType IndexTypeToVk(IndexType type)
		{
			return type == IndexType::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
		}

		// RenderPassDesc 声明的布局 → Vulkan 布局。渲染通道开始/结束时会把**真实**布局
		// 写回 VulkanTexture 的跟踪值,拷贝与描述符采样才有一致的布局事实源。
		VkImageLayout AttachmentLayoutToVk(AttachmentLayout layout)
		{
			switch (layout)
			{
			case AttachmentLayout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case AttachmentLayout::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			case AttachmentLayout::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			case AttachmentLayout::ShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case AttachmentLayout::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			case AttachmentLayout::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case AttachmentLayout::Undefined: break;
			}
			return VK_IMAGE_LAYOUT_UNDEFINED;
		}

		// RHI 资源状态 → Vulkan 布局。PipelineBarrier 用它把"目标状态"翻译成真实转换,
		// 源状态取纹理的跟踪布局(见 VulkanTexture::GetLayout),不再发 GENERAL→GENERAL 空操作。
		VkImageLayout ResourceStateToVk(ResourceState state)
		{
			switch (state)
			{
			case ResourceState::General: return VK_IMAGE_LAYOUT_GENERAL;
			case ResourceState::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			case ResourceState::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			case ResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			case ResourceState::ShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case ResourceState::CopySrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			case ResourceState::CopyDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			case ResourceState::Undefined:
			case ResourceState::VertexBuffer:
			case ResourceState::IndexBuffer:
			case ResourceState::UniformBuffer:
				break;
			}
			return VK_IMAGE_LAYOUT_GENERAL;
		}

		// P4-4a:布局 → 访问标志。只用于 resolve 前后把图像还原到"下一个使用者"的访问语义。
		VkAccessFlags LayoutToAccess(VkImageLayout layout)
		{
			switch (layout)
			{
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
				return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				return VK_ACCESS_SHADER_READ_BIT;
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				return VK_ACCESS_TRANSFER_READ_BIT;
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				return VK_ACCESS_TRANSFER_WRITE_BIT;
			case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
				return VK_ACCESS_MEMORY_READ_BIT;
			default:
				return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			}
		}

		// vkCmdResolveImage 解析深度/模板格式需要 VK_KHR_depth_stencil_resolve(depthStencilResolve 特性)。
		// 本 RHI 的 ResolveTexture 只面向颜色附件;深度附件保持多采样,不解析(与 GL 后端一致)。
		bool IsDepthStencilFormat(Format format)
		{
			return format == Format::D16_UNORM || format == Format::D32_SFLOAT ||
				format == Format::D24_UNORM_S8_UINT || format == Format::D32_SFLOAT_S8_UINT;
		}

		// P4-4a:resolve 专用的布局转换。与 RecordImageLayoutTransition 分开的原因:
		// 后者的语义固定为"拷贝前进入 TRANSFER_SRC / 拷完还原"(还原时 srcAccessMask=TRANSFER_READ),
		// 而 resolve 的写入端是 TRANSFER_WRITE,还原时必须以 TRANSFER_WRITE 作为源域,
		// 否则解析结果对后续采样/读回的可见性没有内存依赖保证。
		void RecordResolveLayoutBarrier(VkCommandBuffer commandBuffer, VulkanTexture& texture,
			VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mip, uint32_t layer,
			VkAccessFlags srcAccess, VkAccessFlags dstAccess)
		{
			if (oldLayout == newLayout)
				return;
			VkImageMemoryBarrier barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = oldLayout;
			barrier.newLayout = newLayout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = texture.GetImage();
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1 };
			barrier.srcAccessMask = srcAccess;
			barrier.dstAccessMask = dstAccess;
			vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
			texture.SetLayout(newLayout);
		}
	}

	VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevice& device, std::string debugName)
		: m_Device(device), m_DebugName(std::move(debugName))
	{
		VkCommandBufferAllocateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		// 用"当前线程"的命令池:多线程录制时命令缓冲必须与其池同线程分配/释放。
		m_Pool = device.GetThreadCommandPool();
		info.commandPool = m_Pool;
		info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		info.commandBufferCount = 1;
		vkAllocateCommandBuffers(device.GetNativeDevice(), &info, &m_CommandBuffer);
	}

	VulkanCommandBuffer::~VulkanCommandBuffer()
	{
		vkFreeCommandBuffers(m_Device.GetNativeDevice(), m_Pool, 1, &m_CommandBuffer);
	}

	void VulkanCommandBuffer::Begin()
	{
		m_PendingDescriptorSets.clear();
		m_TransientBuffers.clear();   // 上一轮同槽位提交的 fence 已通过,临时 staging 可以释放
		m_ActivePassAttachments.clear();
		m_ActivePass = nullptr;
		m_LastPipelineLayout = VK_NULL_HANDLE;
		VkCommandBufferBeginInfo info{};
		info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(m_CommandBuffer, &info);
	}

	void VulkanCommandBuffer::End()
	{
		vkEndCommandBuffer(m_CommandBuffer);
	}

	void VulkanCommandBuffer::BeginLabel(const std::string&) {}
	void VulkanCommandBuffer::EndLabel() {}

	void VulkanCommandBuffer::BeginRenderPass(const Handle<RenderPass>& pass,
		const Handle<Framebuffer>& framebuffer, const std::vector<ClearValue>& clears)
	{
		const auto vulkanPass = std::static_pointer_cast<VulkanRenderPass>(pass);
		const auto vulkanFramebuffer = std::static_pointer_cast<VulkanFramebuffer>(framebuffer);
		std::vector<VkClearValue> clearValues(clears.size());
		for (size_t i = 0; i < clears.size(); ++i)
		{
			if (clears[i].IsDepthStencil)
				clearValues[i].depthStencil = { clears[i].DepthStencil.Depth, clears[i].DepthStencil.Stencil };
			else
				std::memcpy(clearValues[i].color.float32, &clears[i].Color, sizeof(clears[i].Color));
		}
		VkRenderPassBeginInfo info{};
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		info.renderPass = vulkanPass->GetRenderPass();
		info.framebuffer = vulkanFramebuffer->GetFramebuffer();
		info.renderArea.extent = { framebuffer->GetDesc().Extent.Width, framebuffer->GetDesc().Extent.Height };
		info.clearValueCount = static_cast<uint32_t>(clearValues.size());
		info.pClearValues = clearValues.empty() ? nullptr : clearValues.data();
		vkCmdBeginRenderPass(m_CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);

		// 通道开始后附件真实处于声明的 InitialLayout(spec:renderPass 会做隐式转换),
		// 同步给纹理跟踪;结束后再同步 FinalLayout。
		m_ActivePassAttachments.clear();
		m_ActivePass = pass;
		const RenderPassDesc& desc = vulkanPass->GetDesc();
		const std::vector<Handle<Texture>>& attachments = framebuffer->GetDesc().Attachments;
		m_ActivePassAttachments.insert(m_ActivePassAttachments.end(), attachments.begin(), attachments.end());
		const size_t count = std::min(attachments.size(), desc.Attachments.size());
		for (size_t i = 0; i < count; ++i)
			if (const auto texture = std::dynamic_pointer_cast<VulkanTexture>(attachments[i]))
				texture->SetLayout(AttachmentLayoutToVk(desc.Attachments[i].InitialLayout));
	}

	void VulkanCommandBuffer::NextSubpass() { vkCmdNextSubpass(m_CommandBuffer, VK_SUBPASS_CONTENTS_INLINE); }

	void VulkanCommandBuffer::EndRenderPass()
	{
		vkCmdEndRenderPass(m_CommandBuffer);
		// 附件离场后停在 FinalLayout;跟踪必须跟着走,否则后续拷贝/采样会用错布局
		// (实测症状:vkCmdCopyImageToBuffer-srcImageLayout-00189 与 vkCmdDraw-None-09600)。
		if (const auto pass = std::dynamic_pointer_cast<VulkanRenderPass>(m_ActivePass))
		{
			const RenderPassDesc& desc = pass->GetDesc();
			const size_t count = std::min(m_ActivePassAttachments.size(), desc.Attachments.size());
			for (size_t i = 0; i < count; ++i)
				if (const auto texture = std::dynamic_pointer_cast<VulkanTexture>(m_ActivePassAttachments[i]))
					texture->SetLayout(AttachmentLayoutToVk(desc.Attachments[i].FinalLayout));
		}
		m_ActivePassAttachments.clear();
		m_ActivePass = nullptr;
	}

	void VulkanCommandBuffer::SetViewport(const Viewport& viewport)
	{
		VkViewport out{ viewport.X, viewport.Y, viewport.Width, viewport.Height, viewport.MinDepth, viewport.MaxDepth };
		vkCmdSetViewport(m_CommandBuffer, 0, 1, &out);
	}

	void VulkanCommandBuffer::SetScissor(const Scissor& scissor)
	{
		VkRect2D out{ { scissor.X, scissor.Y }, { scissor.Width, scissor.Height } };
		vkCmdSetScissor(m_CommandBuffer, 0, 1, &out);
	}

	void VulkanCommandBuffer::BindPipeline(const Handle<Pipeline>& pipeline)
	{
		if (const auto vulkan = std::dynamic_pointer_cast<VulkanPipeline>(pipeline))
		{
			vkCmdBindPipeline(m_CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vulkan->GetPipeline());
			m_LastPipelineLayout = vulkan->GetLayout();
			for (const auto& [firstSet, descriptorSet] : m_PendingDescriptorSets)
			{
				const auto pendingVulkan = std::dynamic_pointer_cast<VulkanDescriptorSet>(descriptorSet);
				if (!pendingVulkan)
					continue;
				const VkDescriptorSet set = pendingVulkan->GetSet();
				vkCmdBindDescriptorSets(m_CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
					vulkan->GetLayout(), firstSet, 1, &set, 0, nullptr);
			}
			m_PendingDescriptorSets.clear();
		}
	}

	void VulkanCommandBuffer::BindDescriptorSet(const Handle<DescriptorSet>& set, uint32_t firstSet)
	{
		const auto vulkan = std::dynamic_pointer_cast<VulkanDescriptorSet>(set);
		if (!vulkan)
			return;
		if (!m_LastPipelineLayout)
		{
			// Vulkan 需要管线布局才能绑描述符;记录待绑,在下次 BindPipeline 时应用。
			m_PendingDescriptorSets.push_back({ firstSet, set });
			return;
		}
		const VkDescriptorSet descriptorSet = vulkan->GetSet();
		vkCmdBindDescriptorSets(m_CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			m_LastPipelineLayout, firstSet, 1, &descriptorSet, 0, nullptr);
	}

	void VulkanCommandBuffer::BindVertexBuffer(uint32_t binding, const Handle<Buffer>& buffer, uint64_t offset)
	{
		const auto vulkan = std::dynamic_pointer_cast<VulkanBuffer>(buffer);
		if (!vulkan)
			return;
		VkDeviceSize offsets[] = { offset };
		VkBuffer native = vulkan->GetBuffer();
		vkCmdBindVertexBuffers(m_CommandBuffer, binding, 1, &native, offsets);
	}

	void VulkanCommandBuffer::BindIndexBuffer(const Handle<Buffer>& buffer, uint64_t offset, IndexType indexType)
	{
		const auto vulkan = std::dynamic_pointer_cast<VulkanBuffer>(buffer);
		if (vulkan)
			vkCmdBindIndexBuffer(m_CommandBuffer, vulkan->GetBuffer(), offset, IndexTypeToVk(indexType));
	}

	void VulkanCommandBuffer::PushConstants(ShaderStageFlags stages, uint32_t offset, uint32_t size, const void* data)
	{
		vkCmdPushConstants(m_CommandBuffer, m_Device.GetLastPipelineLayout(),
			static_cast<VkShaderStageFlags>(stages), offset, size, data);
	}

	void VulkanCommandBuffer::Draw(uint32_t vertexCount, uint32_t instanceCount,
		uint32_t firstVertex, uint32_t firstInstance)
	{
		vkCmdDraw(m_CommandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
	}

	void VulkanCommandBuffer::DrawIndexed(uint32_t indexCount, uint32_t instanceCount,
		uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
	{
		vkCmdDrawIndexed(m_CommandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}

	void VulkanCommandBuffer::DrawIndirect(const Handle<Buffer>& args, uint64_t offset,
		uint32_t drawCount, uint32_t stride)
	{
		const auto vulkan = std::dynamic_pointer_cast<VulkanBuffer>(args);
		if (vulkan)
			vkCmdDrawIndirect(m_CommandBuffer, vulkan->GetBuffer(), offset, drawCount, stride);
	}

	void VulkanCommandBuffer::DrawIndexedIndirect(const Handle<Buffer>& args, uint64_t offset,
		uint32_t drawCount, uint32_t stride)
	{
		const auto vulkan = std::dynamic_pointer_cast<VulkanBuffer>(args);
		if (vulkan)
			vkCmdDrawIndexedIndirect(m_CommandBuffer, vulkan->GetBuffer(), offset, drawCount, stride);
	}

	void VulkanCommandBuffer::Dispatch(uint32_t groupX, uint32_t groupY, uint32_t groupZ)
	{
		vkCmdDispatch(m_CommandBuffer, groupX, groupY, groupZ);
	}

	void VulkanCommandBuffer::PipelineBarrier(const std::vector<ResourceBarrier>& barriers)
	{
		// 旧实现固定发 GENERAL→GENERAL(等于只做内存依赖、不转换布局),导致跟踪值、
		// 渲染通道真实布局与调用方声明的 Before/After 三方不一致(实测触发
		// VUID-VkImageMemoryBarrier-oldLayout-01197 与 vkCmdDraw-None-09600)。
		// 现在按纹理跟踪的真实布局做转换,并把结果写回跟踪。
		for (const ResourceBarrier& barrier : barriers)
		{
			const auto texture = std::dynamic_pointer_cast<VulkanTexture>(barrier.Texture);
			if (!texture)
				continue;
			const VkImageLayout oldLayout = texture->GetLayout();
			const VkImageLayout newLayout = ResourceStateToVk(barrier.After);
			if (oldLayout == newLayout)
				continue;
			VkImageMemoryBarrier out{};
			out.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			out.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			out.dstAccessMask = newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
				? VK_ACCESS_TRANSFER_READ_BIT
				: (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
					? VK_ACCESS_SHADER_READ_BIT
					: (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
			out.oldLayout = oldLayout;
			out.newLayout = newLayout;
			out.image = texture->GetImage();
			out.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, barrier.BaseMipLevel, barrier.MipLevelCount,
				barrier.BaseArrayLayer, barrier.ArrayLayerCount };
			vkCmdPipelineBarrier(m_CommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &out);
			texture->SetLayout(newLayout);
		}
	}

	void VulkanCommandBuffer::CopyBuffer(const Handle<Buffer>& src, const Handle<Buffer>& dst,
		uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
	{
		const auto srcVk = std::dynamic_pointer_cast<VulkanBuffer>(src);
		const auto dstVk = std::dynamic_pointer_cast<VulkanBuffer>(dst);
		if (srcVk && dstVk)
		{
			VkBufferCopy region{ srcOffset, dstOffset, size };
			vkCmdCopyBuffer(m_CommandBuffer, srcVk->GetBuffer(), dstVk->GetBuffer(), 1, &region);
		}
	}

	void VulkanCommandBuffer::CopyBufferToTexture(const Handle<Buffer>&, const Handle<Texture>&, uint64_t, uint32_t, uint32_t) {}

	void VulkanCommandBuffer::UpdateBuffer(const Handle<Buffer>& dst, const void* data, uint64_t size,
		uint64_t offset)
	{
		const auto dstVk = std::dynamic_pointer_cast<VulkanBuffer>(dst);
		if (!dstVk || !data || size == 0)
			return;
		// vkCmdUpdateBuffer 单次上限 64 KiB;更大走 staging + CopyBuffer。
		constexpr uint64_t kMaxDirectUpdate = 64 * 1024;
		if (size <= kMaxDirectUpdate)
		{
			vkCmdUpdateBuffer(m_CommandBuffer, dstVk->GetBuffer(), offset, size, data);
			return;
		}

		Rhi::BufferDesc stagingDesc;
		stagingDesc.Size = size;
		stagingDesc.Usage = Rhi::BufferUsageTransferSrc;
		stagingDesc.Memory = Rhi::MemoryHint::HostVisible;
		stagingDesc.DebugName = "CmdUpdateStaging";
		Handle<Buffer> staging = m_Device.CreateBuffer(stagingDesc);
		if (!staging)
			return;
		staging->SetData(data, size, 0);
		CopyBuffer(staging, dst, 0, offset, size);
		m_TransientBuffers.push_back(std::move(staging));   // 随命令缓冲复位释放
	}
	void VulkanCommandBuffer::CopyTextureToBuffer(const Handle<Texture>& src, const Handle<Buffer>& dst,
		uint64_t dstOffset, uint32_t mip, uint32_t layer)
	{
		const auto srcVk = std::dynamic_pointer_cast<VulkanTexture>(src);
		const auto dstVk = std::dynamic_pointer_cast<VulkanBuffer>(dst);
		if (!srcVk || !dstVk)
			return;
		// 布局转换由本命令自己负责:调用方不需要(也不应该)依赖 PipelineBarrier —— 后者
		// 目前是 GENERAL→GENERAL 的空操作,而拷贝必须使用 TRANSFER_SRC_OPTIMAL(VUID 00189)。
		// 拷完还原到进入时的布局,保证后续采样/附件使用不受影响。
		const VkImageLayout original = srcVk->GetLayout();
		if (original != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
			RecordImageLayoutTransition(srcVk, original, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mip, layer);
		const TextureDesc& desc = srcVk->GetDesc();
		VkBufferImageCopy region{};
		region.bufferOffset = dstOffset;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = mip;
		region.imageSubresource.baseArrayLayer = layer;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = {
			std::max(1u, desc.Extent.Width >> mip),
			std::max(1u, desc.Extent.Height >> mip),
			std::max(1u, desc.Extent.Depth >> mip) };
		vkCmdCopyImageToBuffer(m_CommandBuffer, srcVk->GetImage(),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstVk->GetBuffer(), 1, &region);
		if (original != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
			RecordImageLayoutTransition(srcVk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, original, mip, layer);
	}

	void VulkanCommandBuffer::RecordImageLayoutTransition(const Handle<Texture>& texture,
		VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mip, uint32_t layer)
	{
		const auto textureVk = std::dynamic_pointer_cast<VulkanTexture>(texture);
		if (!textureVk || oldLayout == newLayout)
			return;

		const bool intoTransferSrc = newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = textureVk->GetImage();
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = mip;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = layer;
		barrier.subresourceRange.layerCount = 1;
		// 语义固定为"拷贝前进入 TRANSFER_SRC / 拷贝后还原":
		// 进入时源域可能来自渲染通道或采样,统一用 ALL_COMMANDS 耗尽旧访问;
		// 还原时源域是刚才的传输读,目的域按要还原到的布局选择,保证下一个使用者看到数据。
		barrier.srcAccessMask = intoTransferSrc ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
			: VK_ACCESS_TRANSFER_READ_BIT;
		switch (newLayout)
		{
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			break;
		default:
			barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			break;
		}
		vkCmdPipelineBarrier(m_CommandBuffer,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			intoTransferSrc ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
	}
	void VulkanCommandBuffer::CopyTexture(const Handle<Texture>&, const Handle<Texture>&, uint32_t, uint32_t, uint32_t, uint32_t) {}

	// P4-4a:显式 MSAA resolve。主路径是渲染通道内的 pResolveAttachments(Vulkan 在 subpass 结束时
	// 由驱动完成 resolve,零额外屏障、零额外命令);本函数是 RHI 对外的显式 resolve 入口
	// (与 GL 后端的 ResolveTexture 对等),语义 = vkCmdResolveImage:
	//   源必须是多采样(否则没有可解析的样本)、目标必须单采样,两者格式必须一致。
	// 因为 vkCmdResolveImage 要求两侧处于 TRANSFER_SRC/DST_OPTIMAL,这里自己做来回转换
	// (调用方不需要额外 PipelineBarrier),并在结束后把两侧还原到进入时的布局。
	void VulkanCommandBuffer::ResolveTexture(const Handle<Texture>& src, const Handle<Texture>& dst,
		uint32_t srcMip, uint32_t dstMip, uint32_t layer)
	{
		const auto srcVk = std::dynamic_pointer_cast<VulkanTexture>(src);
		const auto dstVk = std::dynamic_pointer_cast<VulkanTexture>(dst);
		if (!srcVk || !dstVk || srcVk.get() == dstVk.get())
			return;

		const TextureDesc& srcDesc = srcVk->GetDesc();
		const TextureDesc& dstDesc = dstVk->GetDesc();
		if (srcDesc.Format != dstDesc.Format || IsDepthStencilFormat(srcDesc.Format))
		{
			WLD_CORE_WARN("[RHI-VK] ResolveTexture requires identical, non-depth/stencil formats "
				"(src='{0}' {1} vs dst='{2}' {3}); skipped",
				srcDesc.DebugName, static_cast<int>(srcDesc.Format),
				dstDesc.DebugName, static_cast<int>(dstDesc.Format));
			return;
		}
		if (srcDesc.Samples == SampleCount::Count1 || dstDesc.Samples != SampleCount::Count1)
		{
			WLD_CORE_WARN("[RHI-VK] ResolveTexture needs a multisampled src ('{0}' samples={1}) and a "
				"single-sampled dst ('{2}' samples={3}); skipped",
				srcDesc.DebugName, static_cast<uint32_t>(srcDesc.Samples),
				dstDesc.DebugName, static_cast<uint32_t>(dstDesc.Samples));
			return;
		}

		const VkImageLayout srcOriginal = srcVk->GetLayout();
		const VkImageLayout dstOriginal = dstVk->GetLayout();
		// 进入传输布局:源域"可能是任何上一次写入",统一用 ALL_COMMANDS + 内存读写耗尽;
		// 目的域按具体传输方向给读/写标志。
		RecordResolveLayoutBarrier(m_CommandBuffer, *srcVk, srcOriginal,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcMip, layer,
			VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		RecordResolveLayoutBarrier(m_CommandBuffer, *dstVk, dstOriginal,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dstMip, layer,
			VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

		VkImageResolve region{};
		region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, srcMip, layer, 1 };
		region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, dstMip, layer, 1 };
		region.srcOffset = { 0, 0, 0 };
		region.dstOffset = { 0, 0, 0 };
		// extent 以目标 mip 的尺寸为准(源与目标的 extent 必须兼容;取小值保证不越界)。
		region.extent = {
			std::min(std::max(1u, srcDesc.Extent.Width >> srcMip), std::max(1u, dstDesc.Extent.Width >> dstMip)),
			std::min(std::max(1u, srcDesc.Extent.Height >> srcMip), std::max(1u, dstDesc.Extent.Height >> dstMip)),
			1 };
		vkCmdResolveImage(m_CommandBuffer, srcVk->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dstVk->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		// 还原:源域是刚才的传输读/写,目的域按"原布局的下一个使用者"给标志。
		RecordResolveLayoutBarrier(m_CommandBuffer, *dstVk, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			dstOriginal, dstMip, layer, VK_ACCESS_TRANSFER_WRITE_BIT, LayoutToAccess(dstOriginal));
		RecordResolveLayoutBarrier(m_CommandBuffer, *srcVk, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			srcOriginal, srcMip, layer, VK_ACCESS_TRANSFER_READ_BIT, LayoutToAccess(srcOriginal));
	}

	void VulkanCommandBuffer::GenerateMipmaps(const Handle<Texture>&) {}

	void VulkanCommandBuffer::ResetQueryPool(const Handle<QueryPool>& pool, uint32_t first, uint32_t count)
	{
		const auto vulkan = std::dynamic_pointer_cast<VulkanQueryPool>(pool);
		if (vulkan)
			vkCmdResetQueryPool(m_CommandBuffer, vulkan->GetPool(), first,
				count ? count : vulkan->GetCount() - first);
	}

	void VulkanCommandBuffer::BeginQuery(const Handle<QueryPool>& pool, uint32_t index, QueryType type)
	{
		const auto vulkan = std::dynamic_pointer_cast<VulkanQueryPool>(pool);
		if (vulkan)
			vkCmdBeginQuery(m_CommandBuffer, vulkan->GetPool(), index, 0);
		(void)type;
	}

	void VulkanCommandBuffer::EndQuery(const Handle<QueryPool>& pool, uint32_t index)
	{
		const auto vulkan = std::dynamic_pointer_cast<VulkanQueryPool>(pool);
		if (vulkan)
			vkCmdEndQuery(m_CommandBuffer, vulkan->GetPool(), index);
	}

	void VulkanCommandBuffer::WriteTimestamp(const Handle<QueryPool>& pool, uint32_t index)
	{
		const auto vulkan = std::dynamic_pointer_cast<VulkanQueryPool>(pool);
		if (vulkan)
			vkCmdWriteTimestamp(m_CommandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vulkan->GetPool(), index);
	}

	void VulkanCommandBuffer::CopyQueryResults(const Handle<QueryPool>& pool, const Handle<Buffer>& dst,
		uint32_t first, uint32_t count)
	{
		const auto poolVk = std::dynamic_pointer_cast<VulkanQueryPool>(pool);
		const auto dstVk = std::dynamic_pointer_cast<VulkanBuffer>(dst);
		if (poolVk && dstVk)
			vkCmdCopyQueryPoolResults(m_CommandBuffer, poolVk->GetPool(), first,
				count ? count : poolVk->GetCount() - first, dstVk->GetBuffer(), 0,
				sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
	}

	// ---- Queue ----
	VulkanCommandQueue::VulkanCommandQueue(VulkanDevice& device) : m_Device(device) {}

	void VulkanCommandQueue::Submit(const SubmitInfo& info)
	{
		// 设备已丢失(TDR/reset)时任何提交都只会连锁报错;直接跳过,由宿主决定恢复策略。
		if (m_Device.IsDeviceLost())
			return;
		std::vector<VkCommandBuffer> commandBuffers;
		for (const Handle<CommandBuffer>& commandBuffer : info.CommandBuffers)
			if (const auto vulkan = std::dynamic_pointer_cast<VulkanCommandBuffer>(commandBuffer))
				commandBuffers.push_back(vulkan->GetCommandBuffer());
		std::vector<VkSemaphore> waitSemaphores;
		std::vector<VkPipelineStageFlags> waitStages;
		for (const Handle<Semaphore>& semaphore : info.WaitSemaphores)
			if (const auto vulkan = std::dynamic_pointer_cast<VulkanSemaphore>(semaphore))
			{
				waitSemaphores.push_back(vulkan->GetSemaphore());
				waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			}
		std::vector<VkSemaphore> signalSemaphores;
		for (const Handle<Semaphore>& semaphore : info.SignalSemaphores)
			if (const auto vulkan = std::dynamic_pointer_cast<VulkanSemaphore>(semaphore))
				signalSemaphores.push_back(vulkan->GetSemaphore());

		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
		submit.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
		submit.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
		submit.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());
		submit.pCommandBuffers = commandBuffers.empty() ? nullptr : commandBuffers.data();
		submit.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
		submit.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();
		const VkFence fence = info.Fence
			? std::dynamic_pointer_cast<VulkanFence>(info.Fence)->GetFence() : VK_NULL_HANDLE;
		const VkResult result = vkQueueSubmit(m_Device.GetGraphicsQueue(), 1, &submit, fence);
		// P4-UX5:以前这里**完全没检查**返回值 —— device lost 只会从一次性的 SubmitOneShot 里报出来,
		// 场景/UI 提交死掉时是静默的。现在两条路径都报,并带上命令缓冲名作为出处。
		if (result != VK_SUCCESS)
		{
			std::string names;
			for (const Handle<CommandBuffer>& commandBuffer : info.CommandBuffers)
				if (const auto vulkan = std::dynamic_pointer_cast<VulkanCommandBuffer>(commandBuffer))
				{
					if (!names.empty()) names += ", ";
					names += vulkan->DebugName().empty() ? "<unnamed>" : vulkan->DebugName();
				}
			if (result == VK_ERROR_DEVICE_LOST)
			{
				WLD_CORE_ERROR("[RHI-VK] device lost in queue submit (commandBuffers=[{0}]); GPU work stopped",
					names);
				m_Device.LogDeviceFault(names.empty() ? "queue-submit" : names.c_str());
			}
			else
			{
				WLD_CORE_ERROR("[RHI-VK] vkQueueSubmit failed result={0} (commandBuffers=[{1}])",
					static_cast<int>(result), names);
			}
		}
	}

	void VulkanCommandQueue::WaitIdle()
	{
		if (m_Device.IsDeviceLost())
			return;
		vkQueueWaitIdle(m_Device.GetGraphicsQueue());
	}

	void VulkanCommandQueue::ExecuteImmediate(const std::function<void(CommandBuffer&)>& record)
	{
		if (m_Device.IsDeviceLost())
			return;
		VulkanCommandBuffer buffer(m_Device);
		buffer.Begin();
		record(buffer);
		buffer.End();
		VkCommandBuffer native = buffer.GetCommandBuffer();
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &native;
		vkQueueSubmit(m_Device.GetGraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
		vkQueueWaitIdle(m_Device.GetGraphicsQueue());
	}
}
