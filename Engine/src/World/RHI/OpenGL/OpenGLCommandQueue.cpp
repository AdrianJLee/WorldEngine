#include "wldpch.h"
#include "OpenGLCommandQueue.h"
#include "OpenGLCommandBuffer.h"
#include "OpenGLSync.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	void OpenGLCommandQueue::Submit(const SubmitInfo& info)
	{
		// 延迟命令列表:在渲染线程(持有 GL 上下文)按录制顺序回放。
		for (const Handle<CommandBuffer>& commandBuffer : info.CommandBuffers)
		{
			if (const auto glCommandBuffer = std::dynamic_pointer_cast<OpenGLCommandBuffer>(commandBuffer))
				glCommandBuffer->Replay();
		}
		for (const auto& semaphore : info.WaitSemaphores)
			semaphore->Wait(0);

		if (info.Fence)
			std::static_pointer_cast<OpenGLFence>(info.Fence)->Signal();

		for (const auto& semaphore : info.SignalSemaphores)
			semaphore->Signal(0);
	}

	void OpenGLCommandQueue::WaitIdle()
	{
		glFinish();
	}

	void OpenGLCommandQueue::ExecuteImmediate(const std::function<void(CommandBuffer&)>& record)
	{
		OpenGLCommandBuffer commandBuffer;
		commandBuffer.Begin();
		record(commandBuffer);
		commandBuffer.End();
		// 延迟命令列表:ExecuteImmediate 也必须回放,否则录制的命令会被丢弃。
		commandBuffer.Replay();
	}
}
