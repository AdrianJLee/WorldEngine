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
	}

	VulkanCommandBuffer::VulkanCommandBuffer(VulkanDevice& device) : m_Device(device)
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
	}

	void VulkanCommandBuffer::NextSubpass() { vkCmdNextSubpass(m_CommandBuffer, VK_SUBPASS_CONTENTS_INLINE); }
	void VulkanCommandBuffer::EndRenderPass() { vkCmdEndRenderPass(m_CommandBuffer); }

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
		std::vector<VkImageMemoryBarrier> images;
		for (const ResourceBarrier& barrier : barriers)
		{
			const auto texture = std::dynamic_pointer_cast<VulkanTexture>(barrier.Texture);
			if (!texture)
				continue;
			VkImageMemoryBarrier out{};
			out.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			out.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			out.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			out.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			out.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			out.image = texture->GetImage();
			out.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, barrier.BaseMipLevel, barrier.MipLevelCount,
				barrier.BaseArrayLayer, barrier.ArrayLayerCount };
			images.push_back(out);
		}
		if (!images.empty())
			vkCmdPipelineBarrier(m_CommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(images.size()), images.data());
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
	}
	void VulkanCommandBuffer::CopyTexture(const Handle<Texture>&, const Handle<Texture>&, uint32_t, uint32_t, uint32_t, uint32_t) {}
	void VulkanCommandBuffer::ResolveTexture(const Handle<Texture>&, const Handle<Texture>&, uint32_t, uint32_t, uint32_t) {}
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
		vkQueueSubmit(m_Device.GetGraphicsQueue(), 1, &submit, fence);
	}

	void VulkanCommandQueue::WaitIdle()
	{
		vkQueueWaitIdle(m_Device.GetGraphicsQueue());
	}

	void VulkanCommandQueue::ExecuteImmediate(const std::function<void(CommandBuffer&)>& record)
	{
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
