#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	class RenderPass;
	class Framebuffer;
	class Pipeline;
	class DescriptorSet;
	class Buffer;
	class Texture;

	class WLD_API CommandBuffer
	{
	public:
		virtual ~CommandBuffer() = default;

		virtual void Begin() = 0;
		virtual void End() = 0;

		virtual void BeginRenderPass(const Handle<RenderPass>& pass,
			const Handle<Framebuffer>& framebuffer,
			const std::vector<ClearValue>& clears) = 0;
		virtual void NextSubpass() = 0;
		virtual void EndRenderPass() = 0;

		virtual void SetViewport(const Viewport& viewport) = 0;
		virtual void SetScissor(const Scissor& scissor) = 0;

		virtual void BindPipeline(const Handle<Pipeline>& pipeline) = 0;
		virtual void BindDescriptorSet(const Handle<DescriptorSet>& set, uint32_t firstSet = 0) = 0;
		virtual void BindVertexBuffer(uint32_t binding, const Handle<Buffer>& buffer, uint64_t offset = 0) = 0;
		virtual void BindIndexBuffer(const Handle<Buffer>& buffer, uint64_t offset = 0, IndexType indexType = IndexType::UInt32) = 0;
		virtual void PushConstants(ShaderStageFlags stages, uint32_t offset, uint32_t size, const void* data) = 0;

		virtual void Draw(uint32_t vertexCount, uint32_t instanceCount = 1,
			uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;
		virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1,
			uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) = 0;
		virtual void DrawIndirect(const Handle<Buffer>& args, uint64_t offset,
			uint32_t drawCount, uint32_t stride) = 0;
		virtual void DrawIndexedIndirect(const Handle<Buffer>& args, uint64_t offset,
			uint32_t drawCount, uint32_t stride) = 0;
		virtual void Dispatch(uint32_t groupX, uint32_t groupY = 1, uint32_t groupZ = 1) = 0;

		virtual void PipelineBarrier(const std::vector<ResourceBarrier>& barriers) = 0;
		virtual void CopyBuffer(const Handle<Buffer>& src, const Handle<Buffer>& dst,
			uint64_t srcOffset, uint64_t dstOffset, uint64_t size) = 0;
		virtual void CopyBufferToTexture(const Handle<Buffer>& src, const Handle<Texture>& dst,
			uint64_t srcOffset, uint32_t mip = 0, uint32_t layer = 0) = 0;
		virtual void CopyTextureToBuffer(const Handle<Texture>& src, const Handle<Buffer>& dst,
			uint64_t dstOffset, uint32_t mip = 0, uint32_t layer = 0) = 0;
	};
}
