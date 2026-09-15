#pragma once

#include "World/RHI/RhiCommandBuffer.h"
#include "World/RHI/RhiCommandQueue.h"

#include <volk.h>

namespace World::Rhi::Vulkan
{
	class VulkanDevice;

	class VulkanCommandBuffer final : public CommandBuffer
	{
	public:
		VulkanCommandBuffer(VulkanDevice& device);
		~VulkanCommandBuffer() override;

		void Begin() override;
		void End() override;
		void BeginLabel(const std::string& label) override;
		void EndLabel() override;
		void BeginRenderPass(const Handle<RenderPass>& pass, const Handle<Framebuffer>& framebuffer,
			const std::vector<ClearValue>& clears) override;
		void NextSubpass() override;
		void EndRenderPass() override;
		void SetViewport(const Viewport& viewport) override;
		void SetScissor(const Scissor& scissor) override;
		void BindPipeline(const Handle<Pipeline>& pipeline) override;
		void BindDescriptorSet(const Handle<DescriptorSet>& set, uint32_t firstSet = 0) override;
		void BindVertexBuffer(uint32_t binding, const Handle<Buffer>& buffer, uint64_t offset = 0) override;
		void BindIndexBuffer(const Handle<Buffer>& buffer, uint64_t offset = 0, IndexType indexType = IndexType::UInt32) override;
		void PushConstants(ShaderStageFlags stages, uint32_t offset, uint32_t size, const void* data) override;
		void Draw(uint32_t vertexCount, uint32_t instanceCount = 1,
			uint32_t firstVertex = 0, uint32_t firstInstance = 0) override;
		void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1,
			uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) override;
		void DrawIndirect(const Handle<Buffer>& args, uint64_t offset,
			uint32_t drawCount, uint32_t stride) override;
		void DrawIndexedIndirect(const Handle<Buffer>& args, uint64_t offset,
			uint32_t drawCount, uint32_t stride) override;
		void Dispatch(uint32_t groupX, uint32_t groupY = 1, uint32_t groupZ = 1) override;
		void PipelineBarrier(const std::vector<ResourceBarrier>& barriers) override;
		void CopyBuffer(const Handle<Buffer>& src, const Handle<Buffer>& dst,
			uint64_t srcOffset, uint64_t dstOffset, uint64_t size) override;
		void UpdateBuffer(const Handle<Buffer>& dst, const void* data, uint64_t size,
			uint64_t offset = 0) override;
		void CopyBufferToTexture(const Handle<Buffer>& src, const Handle<Texture>& dst,
			uint64_t srcOffset, uint32_t mip = 0, uint32_t layer = 0) override;
		void CopyTextureToBuffer(const Handle<Texture>& src, const Handle<Buffer>& dst,
			uint64_t dstOffset, uint32_t mip = 0, uint32_t layer = 0) override;
		void CopyTexture(const Handle<Texture>& src, const Handle<Texture>& dst,
			uint32_t srcMip = 0, uint32_t srcLayer = 0,
			uint32_t dstMip = 0, uint32_t dstLayer = 0) override;
		void ResolveTexture(const Handle<Texture>& src, const Handle<Texture>& dst,
			uint32_t srcMip = 0, uint32_t dstMip = 0, uint32_t layer = 0) override;
		void GenerateMipmaps(const Handle<Texture>& texture) override;
		void ResetQueryPool(const Handle<QueryPool>& pool, uint32_t first = 0, uint32_t count = 0) override;
		void BeginQuery(const Handle<QueryPool>& pool, uint32_t index, QueryType type = QueryType::Occlusion) override;
		void EndQuery(const Handle<QueryPool>& pool, uint32_t index) override;
		void WriteTimestamp(const Handle<QueryPool>& pool, uint32_t index) override;
		void CopyQueryResults(const Handle<QueryPool>& pool, const Handle<Buffer>& dst,
			uint32_t first = 0, uint32_t count = 0) override;

		VkCommandBuffer GetCommandBuffer() const { return m_CommandBuffer; }
	private:
		VulkanDevice& m_Device;
		VkCommandBuffer m_CommandBuffer = VK_NULL_HANDLE;
		VkCommandPool m_Pool = VK_NULL_HANDLE;   // 分配该命令缓冲的线程池
		VkPipelineLayout m_LastPipelineLayout = VK_NULL_HANDLE;
		std::vector<std::pair<uint32_t, Handle<DescriptorSet>>> m_PendingDescriptorSets;
		// UpdateBuffer 走 staging 时临时创建的源缓冲:复用到本槽位时(两帧后)释放。
		std::vector<Handle<Buffer>> m_TransientBuffers;
	};

	class VulkanCommandQueue final : public CommandQueue
	{
	public:
		VulkanCommandQueue(VulkanDevice& device);
		void Submit(const SubmitInfo& info) override;
		void WaitIdle() override;
		void ExecuteImmediate(const std::function<void(CommandBuffer&)>& record) override;
	private:
		VulkanDevice& m_Device;
	};
}
