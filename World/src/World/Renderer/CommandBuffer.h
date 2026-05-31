#pragma once
#include "RenderPass.h"
#include "PipelineStateObject.h"
#include "VertexArray.h"
#include "DescriptorSet.h"
namespace World
{
	class CommandBuffer
	{
	public:
		static Ref<CommandBuffer> Create();

	public:
		virtual ~CommandBuffer() = default;
		virtual void AddCommand(const std::function<void()>& command) = 0;

		virtual void Begin(uint32_t frameIndex) = 0;
		virtual void End() = 0;
		virtual void StartBatch() = 0;
		// 录制指令
		virtual void BeginRenderPass(Ref<RenderPass> renderPass, bool clear) = 0;
		virtual void BeginRenderPass(Ref<RenderPass> renderPass) = 0;
		virtual void EndRenderPass() = 0;

		virtual void BindPipeline(Ref<PipelineStateObject> pipeline) = 0;
		virtual void DrawIndexed(Ref<VertexArray> va, uint32_t count = 0) = 0;
		virtual void DrawLines(Ref<VertexArray> va, uint32_t vertexCount) = 0;
		virtual void Execute() = 0;

		virtual void BindDescriptorSet(Ref<DescriptorSet> descriptorSet) = 0;

		virtual void SetBufferData(Ref<class VertexBuffer> vertexBuffer, const void* data, uint32_t size) = 0;
		virtual uint32_t GetCurrentFrameIndex() const = 0;
		// ... 其他 Draw 指令

	};
}