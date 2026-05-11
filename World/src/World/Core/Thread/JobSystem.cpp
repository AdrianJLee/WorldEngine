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

		// 留出一个队列给主线程使用
		m_Queues = new JobQueue[numThreads + 1];

		for (uint32_t i = 0; i < numThreads; ++i)
		{
			// Lambda表达式创建线程
			m_Workers.emplace_back([i]()
				{
					m_LocalIndex = i;
					WorkerLoop();
				});
		}
		// 主线程使用最后一个队列
		m_LocalIndex = numThreads;
	}
	void JobSystem::Kick(JobDecl job, JobCounter* counter)
	{
		// 记录任务计数器，通常在提交任务时会关联一个计数器，以便后续等待时知道还有多少任务未完成
		if (counter)
			counter->Count.fetch_add(1);// 增加计数器，使用原子操作确保线程安全
		m_Queues[m_LocalIndex].Push(job);
	}
	void JobSystem::Wait(JobCounter* counter)
	{
		while (counter->Count.load(std::memory_order_acquire) > 0)
		{
			JobDecl job;
			if (GetNextJob(job))
			{
				job.Entry(job.Data);

				counter->Count.fetch_sub(1, std::memory_order_release);
			}
			else
			{
				// 没有任务可执行了，主动让出 CPU 给其他线程，避免忙等待导致的 CPU 占用过高
				std::this_thread::yield();
			}
		}
	}
	bool JobSystem::GetNextJob(JobDecl& outJob)
	{
		// 先尝试从本线程队列拿
		if (m_Queues[m_LocalIndex].Pop(outJob)) return true;

		// 拿不到就去偷别人的
		for (uint32_t i = 0; i <= m_NumWorkers; ++i)
		{
			if (i == m_LocalIndex) continue;
			if (m_Queues[i].Steal(outJob)) return true;
		}
		return false;
	}
	void JobSystem::WorkerLoop()
	{
		while (m_Running)
		{
			JobDecl job;
			if (GetNextJob(job))
			{
				job.Entry(job.Data);
				// 如果任务有关联的计数器，通常在任务逻辑内部减一
			}
			else
			{
				// 让线程休眠100微秒
				std::this_thread::sleep_for(std::chrono::microseconds(100)); // 降频节能
			}
		}
	}
}