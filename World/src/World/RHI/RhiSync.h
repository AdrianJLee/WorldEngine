#pragma once

#include "World/RHI/RhiCore.h"

namespace World::Rhi
{
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
	};
}
