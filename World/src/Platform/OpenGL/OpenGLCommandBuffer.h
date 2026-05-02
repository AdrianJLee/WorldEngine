#pragma once
#include "World/Renderer/CommandBuffer.h"
namespace World
{
	class OpenGLCommandBuffer :public CommandBuffer
	{
	public:
		virtual ~OpenGLCommandBuffer() = default;

		virtual void Begin(uint32_t frameIndex) override;
		virtual void End() override;

		virtual void BeginRenderPass(Ref<RenderPass> renderPass, bool clear) override;
		virtual void EndRenderPass() override;

		virtual void BindPipeline(Ref<PipelineStateObject> pipeline) override;
		virtual void DrawIndexed(Ref<VertexArray> va, uint32_t count = 0) override;
		virtual void DrawLines(Ref<VertexArray> va, uint32_t vertexCount) override;
		virtual void Execute() override;

		virtual void BindDescriptorSet(Ref<DescriptorSet> descriptorSet) override;

		virtual void SetBufferData(Ref<class VertexBuffer> vertexBuffer, const void* data, uint32_t size) override;
	private:
		std::vector<std::function<void()>> m_CommandQueue;
		uint32_t m_CurrentFrameIndex = 0;
	};
}