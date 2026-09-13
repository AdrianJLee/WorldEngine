#include "wldpch.h"
#include "OpenGLCommandQueue.h"
#include "OpenGLCommandBuffer.h"
#include "OpenGLSync.h"

#include <glad/glad.h>

namespace World::Rhi::OpenGL
{
	void OpenGLCommandQueue::Submit(const SubmitInfo& info)
	{
		// GL 立即模式:命令缓冲在录制时已执行。这里只处理同步原语。
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
	}
}
