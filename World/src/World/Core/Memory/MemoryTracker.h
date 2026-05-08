#pragma once
#include "Memory.h"

namespace World
{
	// 增加分配器类型枚举，方便 UI 分类显示
	enum class AllocatorType
	{
		Unknown = 0,
		Linear,
		Stack,
		Pool,
		DualTrack
	};

	// 通用的分配器统计快照
	struct AllocatorStats
	{
		const char* Name;         // 分配器实例名称
		AllocatorType Type;       // 分配器类型
		size_t UsedBytes;         // 已使用字节
		size_t TotalReserved;     // 总预留字节
		size_t NumAllocations;    // 活跃分配数
	};

	class MemoryTracker
	{
	public:
		static MemoryTracker& Get()
		{
			static MemoryTracker instance;
			return instance;
		}

		// 核心：注册函数现在接受 Allocator 基类
		void Register(Allocator* allocator, const char* name, AllocatorType type)
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			// 我们需要一种方式存储这些元数据，可以直接存入分配器或使用辅助结构
			m_Allocators.push_back({ allocator, name, type });
		}

		void Unregister(Allocator* allocator)
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Allocators.erase(std::remove_if(m_Allocators.begin(), m_Allocators.end(),
				[allocator](const TrackerEntry& entry) { return entry.Ptr == allocator; }),
				m_Allocators.end());
		}

		// 获取所有分配器的快照
		std::vector<AllocatorStats> GetFullSnapshot()
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

	private:
		struct TrackerEntry
		{
			Allocator* Ptr;
			const char* Name;
			AllocatorType Type;
		};

		std::vector<TrackerEntry> m_Allocators;
		std::mutex m_Mutex;
	};
}