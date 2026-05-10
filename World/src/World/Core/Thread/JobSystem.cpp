#include "wldpch.h"
#include "JobSystem.h"
#include "JobQueue.h"

namespace World
{
	void JobSystem::Init()
	{
		// 获得系统的 CPU 核心数量，减去主线程
		uint32_t numThreads = std::thread::hardware_concurrency() - 1;
		m_NumWorkers = numThreads;
		m_Queues = new JobQueue[numThreads + 1];

		for (uint32_t i = 0; i < numThreads; ++i)
		{
			m_Workers.emplace_back([i]()
				{
					//WorkerThreadMain(i);
				});
		}
	}
	bool JobSystem::GetNextJob(JobDecl& outJob)
	{
		return false;
	}
	void JobSystem::WorkerLoop()
	{
		while (m_Running)
		{
			JobDecl job;
		}
	}
}