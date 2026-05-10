#pragma once
#include "Thread.h"

namespace World
{
	class JobQueue;
	struct JobDecl;

	class JobSystem
	{
	public:
		static void Init();
	private:
		static bool GetNextJob(JobDecl& outJob);
		static void WorkerLoop();
	private:
		static inline thread_local uint32_t m_LocalIndex = 0; // 每个线程的本地索引，主线程为0，工作线程从1开始
		static inline std::vector<std::thread> m_Workers; // 工作线程列表
		static inline JobQueue* m_Queues = nullptr; // 作业队列
		static inline uint32_t m_NumWorkers = 0; // 工作线程数量
		static inline bool m_Running = true; // 作业系统运行状态
	};
}
