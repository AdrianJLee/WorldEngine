#include "wldpch.h"
#include "MemoryTracker.h"
#include "Memory.h"
namespace World
{
	void MemoryTracker::Register(Allocator* allocator, const char* name, AllocatorType type)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		// 我们需要一种方式存储这些元数据，可以直接存入分配器或使用辅助结构
		m_Allocators.push_back({ allocator, name, type });
	}

	void MemoryTracker::Unregister(Allocator* allocator)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Allocators.erase(std::remove_if(m_Allocators.begin(), m_Allocators.end(),
			[allocator](const TrackerEntry& entry) { return entry.Ptr == allocator; }),
			m_Allocators.end());
	}
	std::vector<AllocatorStats> MemoryTracker::GetFullSnapshot()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::vector<AllocatorStats> stats;
		for (auto& entry : m_Allocators)
		{
			stats.push_back({
				entry.Name,
				entry.Type,
				entry.Ptr->GetUsedMemory(),
				entry.Ptr->GetSize(),
				entry.Ptr->GetNumAllocations()
				});
		}
		return stats;
	}
}
