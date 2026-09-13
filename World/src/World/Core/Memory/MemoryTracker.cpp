#include "wldpch.h"
#include "MemoryTracker.h"
#include "Memory.h"
namespace World
{
	MemoryTracker& MemoryTracker::Get()
	{
		static MemoryTracker instance;
		return instance;
	}

	void MemoryTracker::Register(Allocator* allocator, const char* name, AllocatorType type, bool isEphemeral)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		// 我们需要一种方式存储这些元数据，可以直接存入分配器或使用辅助结构
		m_Allocators.push_back({ allocator, name, type, isEphemeral });
	}

	void MemoryTracker::Unregister(Allocator* allocator)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = std::find_if(m_Allocators.begin(), m_Allocators.end(),
			[allocator](const TrackerEntry& entry) { return entry.Ptr == allocator; });

		if (it != m_Allocators.end())
		{
			if (it->IsEphemeral)
			{
				// 安全快照：使用记录的巅峰值或现有值，不要去调用涉及运算的函数
				// 因为此时 allocator 可能已经处于析构的受损状态！
				m_EphemeralDeadStats.push_back({
					it->Name,
					it->Type,
					allocator->GetUsedMemory(), // 如果你没加峰值可以直接这样，但要确保这之前没被归零
					allocator->GetSize(),
					allocator->GetNumAllocations()
					});
			}
			m_Allocators.erase(it);
		}
	}
	void MemoryTracker::AddEphemeralSnapshot(const Allocator* allocator)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = std::find_if(m_Allocators.begin(), m_Allocators.end(),
			[allocator](const TrackerEntry& entry) { return entry.Ptr == allocator; });

		if (it != m_Allocators.end())
		{
			if (it->IsEphemeral)
			{
				// 安全快照：使用记录的巅峰值或现有值，不要去调用涉及运算的函数
				// 因为此时 allocator 可能已经处于析构的受损状态！
				m_EphemeralDeadStats.push_back({
					it->Name,
					it->Type,
					allocator->GetUsedMemory(), // 如果你没加峰值可以直接这样，但要确保这之前没被归零
					allocator->GetSize(),
					allocator->GetNumAllocations()
					});
			}
			m_Allocators.erase(it);
		}
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

		for (auto& entry : m_EphemeralDeadStats)
		{
			stats.push_back(entry);
		}

		return stats;
	}

	void MemoryTracker::ClearEphemeralStats()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_EphemeralDeadStats.clear();
	}
}
