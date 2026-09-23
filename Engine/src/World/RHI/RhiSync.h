#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
	struct SemaphoreCreateDesc
	{
		bool Timeline = false;
		uint64_t InitialValue = 0;
	};

	class WLD_API Fence
	{
	public:
		virtual ~Fence() = default;
		virtual void Wait(uint64_t timeoutNs = UINT64_MAX) = 0;   // UINT64_MAX = 无限
		virtual bool IsSignaled() const = 0;
		virtual void Reset() = 0;
	};

	class WLD_API Semaphore
	{
	public:
		virtual ~Semaphore() = default;
		// 时间线信号量:Wait/Signal 使用数值;二值信号量忽略数值。
		virtual void Signal(uint64_t value = 0) = 0;
		virtual void Wait(uint64_t value = 0) = 0;
		virtual bool IsTimeline() const = 0;
	};

	class WLD_API QueryPool
	{
	public:
		virtual ~QueryPool() = default;
		virtual QueryType GetType() const = 0;
		virtual uint32_t GetCount() const = 0;
	};
}
