#include "wldpch.h"
#include "MemoryTracker.h"

#include "PoolAllocator.h"
namespace World
{
	void MemoryTracker::Unregister(PoolAllocator* allocator)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = std::find(m_Allocators.begin(), m_Allocators.end(), allocator);
		if (it != m_Allocators.end())
		{
			m_Allocators.erase(it);
		}
	}
	std::vector<PoolStats> MemoryTracker::GetSnapshot()
	{
		std::vector<PoolStats> snapshot;
		std::lock_guard<std::mutex> lock(m_Mutex); // 保护的是 m_Allocators 列表，不是数据本身

		snapshot.reserve(m_Allocators.size());
		for (auto* alloc : m_Allocators)
		{
			snapshot.push_back(alloc->GetStats());
		}
		return snapshot;
	}
}