#pragma once
#include "Allocator.h"
namespace World
{
	class MemoryTracker
	{
	public:
		static MemoryTracker& Get();

		// 核心：注册函数现在接受 Allocator 基类
		void Register(Allocator* allocator, const char* name, AllocatorType type, bool isEphemeral = false);


		void Unregister(Allocator* allocator);
		void AddEphemeralSnapshot(const Allocator* allocator);

		// 获取所有分配器的快照
		std::vector<AllocatorStats> GetFullSnapshot();

		void ClearEphemeralStats();
	private:
		struct TrackerEntry
		{
			Allocator* Ptr;
			const char* Name;
			AllocatorType Type;
			bool IsEphemeral = false;
		};

		std::vector<TrackerEntry> m_Allocators;

		// 存放一帧之内已经被释放/Unregister的短命分配器的死亡快照
		std::vector<AllocatorStats> m_EphemeralDeadStats;
		std::mutex m_Mutex;
	};
}
