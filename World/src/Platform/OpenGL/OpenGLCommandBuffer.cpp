#include "wldpch.h"
#include "OpenGLCommandBuffer.h"
#include "World/Renderer/RenderCommand.h"
#include "World/Renderer/Renderer2D.h"
namespace World
{

	void OpenGLCommandBuffer::Begin(uint32_t frameIndex)
	{
		m_CommandQueue.clear();
		m_CurrentFrameIndex = frameIndex;
	}
	void OpenGLCommandBuffer::End()
	{
		// 录制结束，此时指令已全部存入 m_CommandQueue
	}
	void OpenGLCommandBuffer::StartBatch()
	{
		m_CommandQueue.push_back([]()
			{
				Renderer2D::StartBatch();
			});
	}
	void OpenGLCommandBuffer::BeginRenderPass(Ref<RenderPass> renderPass, bool clear)
	{
		m_CommandQueue.push_back([renderPass, clear]()
			{
				RenderCommand::BeginRenderPass(renderPass, clear);
			});
	}
	void OpenGLCommandBuffer::EndRenderPass()
	{
		m_CommandQueue.push_back([]()
			{
				RenderCommand::EndRenderPass();
			});
	}
	void OpenGLCommandBuffer::BindPipeline(Ref<PipelineStateObject> pipeline)
	{
		m_CommandQueue.push_back([pipeline]()
			{
				pipeline->Bind();
			});
	}
	void OpenGLCommandBuffer::DrawIndexed(Ref<VertexArray> va, uint32_t count)
	{
		m_CommandQueue.push_back([va, count]()
			{
				RenderCommand::DrawIndexed(va, count); // 最终调用 glDrawElements
			});
	}
	void OpenGLCommandBuffer::DrawLines(Ref<VertexArray> va, uint32_t vertexCount)
	{
		m_CommandQueue.push_back([va, vertexCount]()
			{
				RenderCommand::DrawLines(va, vertexCount);
			});
	}
	void OpenGLCommandBuffer::Execute()
	{
		WLD_PROFILE_FUNCTION();
		// 核心：在渲染线程按序回放所有指令
		for (auto& command : m_CommandQueue)
		{
			command();
		}
	}
	void OpenGLCommandBuffer::BindDescriptorSet(Ref<DescriptorSet> descriptorSet)
	{
		uint32_t frameIndex = m_CurrentFrameIndex;
		m_CommandQueue.push_back([descriptorSet, frameIndex]()
			{
				descriptorSet->Bind(frameIndex);
			});
	}
	void OpenGLCommandBuffer::SetBufferData(Ref<class VertexBuffer> vertexBuffer, const void* data, uint32_t size)
	{
		void* dataCopy = malloc(size);
		memcpy(dataCopy, data, size);
		m_CommandQueue.push_back([vertexBuffer, dataCopy, size]()
			{
				// 2. 这里是“执行”时刻（Execute）
				vertexBuffer->SetData(dataCopy, size);
				free(dataCopy);
			});
		//m_CommandQueue.push_back([vertexBuffer, data, size]()
		//	{
		//		vertexBuffer->SetData(data, size);
		//	});
	}
}