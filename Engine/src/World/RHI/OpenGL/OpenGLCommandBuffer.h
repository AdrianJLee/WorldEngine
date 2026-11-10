#pragma once

#include "World/RHI/RhiCommandBuffer.h"

#include <glad/glad.h>

#include <cstdint>
#include <string>
#include <vector>

namespace World::Rhi::OpenGL
{
	class OpenGLPipeline;

	// 延迟命令列表:录制期只写数据,回放期在渲染线程执行(GL 语义下的"多线程录制")。
	enum class GLCommandKind : uint8_t
	{
		BeginLabel, EndLabel,
		BeginRenderPass, EndRenderPass,
		SetViewport, SetScissor,
		BindPipeline, BindDescriptorSet, BindVertexBuffer, BindIndexBuffer,
		Draw, DrawIndexed, DrawIndirect, DrawIndexedIndirect, Dispatch,
		PipelineBarrier, CopyBuffer, UpdateBuffer,
		CopyBufferToTexture, CopyTextureToBuffer, CopyTexture, ResolveTexture, GenerateMipmaps,
		ResetQueryPool, BeginQuery, EndQuery, WriteTimestamp, CopyQueryResults,
	};

	struct GLCommand
	{
		GLCommandKind Kind = GLCommandKind::BeginLabel;
		Handle<Pipeline> Pipeline_;
		Handle<DescriptorSet> DescriptorSet_;
		Handle<Buffer> BufferA, BufferB;
		Handle<Texture> TextureA, TextureB;
		Handle<RenderPass> Pass;
		Handle<Framebuffer> Framebuffer_;
		Handle<QueryPool> QueryPool_;
		std::vector<ClearValue> Clears;
		std::vector<ResourceBarrier> Barriers;
		std::vector<uint8_t> Data;        // UpdateBuffer 的数据副本
		Viewport Viewport_;
		Scissor Scissor_;
		uint64_t OffsetA = 0, OffsetB = 0, Size = 0;
		uint32_t Count0 = 0, Count1 = 0, Count2 = 0, Count3 = 0;
		int32_t Signed0 = 0;
		uint32_t Binding = 0;
		uint32_t FirstSet = 0;
		IndexType IndexType_ = IndexType::UInt32;
		QueryType QueryType_ = QueryType::Occlusion;
		ShaderStageFlags Stages = 0;
		std::string Label;
	};

	class OpenGLCommandBuffer : public CommandBuffer
	{
	public:
		OpenGLCommandBuffer() = default;

		// 在渲染线程回放已录制的命令(由 OpenGLCommandQueue::Submit 调用)。
		void Replay();
		bool IsRecording() const { return m_Recording; }
		size_t CommandCount() const { return m_Commands.size(); }

		void Begin() override;
		void End() override;

		void BeginLabel(const std::string& label) override;
		void EndLabel() override;

		void BeginRenderPass(const Handle<RenderPass>& pass,
			const Handle<Framebuffer>& framebuffer,
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
		void CopyQueryResults(const Handle<QueryPool>& pool,
			const Handle<Buffer>& dst, uint32_t first = 0, uint32_t count = 0) override;

	private:
		GLCommand& Push(GLCommandKind kind);

		std::vector<GLCommand> m_Commands;
		bool m_Recording = false;
		bool m_PushConstantsWarned = false;
	};
}
