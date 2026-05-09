#pragma once
#include "Allocator.h"
namespace World
{
	class MemoryTracker
	{
	public:
		static MemoryTracker& Get()
		{
			static MemoryTracker instance;
			return instance;
		}

		// 核心：注册函数现在接受 Allocator 基类
		void Register(Allocator* allocator, const char* name, AllocatorType type);


		void Unregister(Allocator* allocator);

		// 获取所有分配器的快照
		std::vector<AllocatorStats> GetFullSnapshot();


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