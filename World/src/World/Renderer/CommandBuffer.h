#pragma once
#include "RenderPass.h"
#include "PipelineStateObject.h"
#include "VertexArray.h"
namespace World
{
	class CommandBuffer
	{
	public:
		static Ref<CommandBuffer> Create();
	public:
		virtual ~CommandBuffer() = default;

		virtual void Begin() = 0;
		virtual void End() = 0;

		// 录制指令
		virtual void BeginRenderPass(Ref<RenderPass> renderPass) = 0;
		virtual void EndRenderPass() = 0;

		virtual void BindPipeline(Ref<PipelineStateObject> pipeline) = 0;
		virtual void DrawIndexed(Ref<VertexArray> va, uint32_t count = 0) = 0;
		virtual void DrawLines(Ref<VertexArray> va, uint32_t vertexCount) = 0;
		virtual void Execute() = 0;

		// ... 其他 Draw 指令

	};
}