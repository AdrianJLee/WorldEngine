#pragma once

#include "World/RHI/RhiCore.h"

#include <functional>

namespace World::Rhi
{
	class CommandBuffer;

	struct SubmitInfo
	{
		std::vector<Handle<CommandBuffer>> CommandBuffers;
		std::vector<Handle<class Semaphore>> WaitSemaphores;    // 等待完成
		std::vector<Handle<class Semaphore>> SignalSemaphores;  // 完成后发信号
		Handle<class Fence> Fence;                              // 可选
	};

	class WLD_API CommandQueue
	{
	public:
		virtual ~CommandQueue() = default;
		virtual void Submit(const SubmitInfo& info) = 0;
		virtual void WaitIdle() = 0;
		// 立即执行一次性上传/下载(拷贝队列可用时后端自行选队)。
		virtual void ExecuteImmediate(const std::function<void(CommandBuffer&)>& record) = 0;
	};
}
