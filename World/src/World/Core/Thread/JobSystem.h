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

		// 提交一个任务包
		static void Kick(JobDecl job, JobCounter* counter = nullptr);

		// 核心：非阻塞等待（边等边干活）
		static void Wait(JobCounter* counter);
	private:
		// 获取下一个可执行的任务，优先从本地队列拿，拿不到就去偷别人的
		static bool GetNextJob(JobDecl& outJob);

		// 工作线程主循环
		static void WorkerLoop();
	private:
		static inline thread_local uint32_t m_LocalIndex = 0; // 线程局部索引，主线程为最后一个
		static inline std::vector<std::thread> m_Workers; // 工作线程列表
		static inline JobQueue* m_Queues = nullptr; // 作业队列
		static inline uint32_t m_NumWorkers = 0; // 工作线程数量
		static inline bool m_Running = true; // 作业系统运行状态
	};
}
