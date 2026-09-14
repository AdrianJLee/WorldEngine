#pragma once
#include "Allocator.h"

#include <mutex>
#include <string>
#include <vector>
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

		// 泄漏检查:返回"仍有活跃分配"的分配器数量,并把明细写入 out(可空)。
		// 供退出/关卡卸载时做断言或日志;不计入短命分配器的死亡快照。
		size_t ReportLeaks(std::vector<std::string>* out = nullptr);

		void ClearEphemeralStats();
	private:
		struct TrackerEntry
		{
			Allocator* Ptr;
			const char* Name;
			AllocatorType Type;
			bool IsEphemeral = false;
			size_t PeakBytes = 0;
		};

		std::vector<TrackerEntry> m_Allocators;

		// 存放一帧之内已经被释放/Unregister的短命分配器的死亡快照
		std::vector<AllocatorStats> m_EphemeralDeadStats;
		std::mutex m_Mutex;
	};
}


