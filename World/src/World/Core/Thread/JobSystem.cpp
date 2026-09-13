#include "wldpch.h"
#include "JobSystem.h"
#include "JobQueue.h"
#include "World/Core/Memory/LinearAllocator.h"
namespace World
{
	std::vector<std::thread> JobSystem::m_Workers;
	JobQueue* JobSystem::m_Queues = nullptr;
	uint32_t JobSystem::m_NumWorkers = 0;
	std::atomic<bool> JobSystem::m_Running { true };

	uint32_t& JobSystem::LocalIndex()
	{
		static thread_local uint32_t index = 0;
		return index;
	}

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
					LocalIndex() = i;
					WorkerLoop();
				});
		}
		// 主线程使用最后一个队列
		LocalIndex() = numThreads;
	}
	void JobSystem::Kick(JobDecl job)
	{
		// 记录任务计数器，通常在提交任务时会关联一个计数器，以便后续等待时知道还有多少任务未完成
		if (job.Counter)
			job.Counter->Count.fetch_add(1);// 增加计数器，使用原子操作确保线程安全

		m_Queues[LocalIndex()].Push(job);
	}
	void JobSystem::Wait(JobCounter* counter)
	{
		while (counter->Count.load(std::memory_order_acquire) > 0)
		{
			JobDecl job;
			if (GetNextJob(job))
			{
				job.Entry(job.Padding);

				if (job.Counter)
				{
					job.Counter->Count.fetch_sub(1, std::memory_order_release);
				}
			}
			else
			{
				// 没有任务可执行了，主动让出 CPU 给其他线程，避免忙等待导致的 CPU 占用过高
				std::this_thread::yield();
			}
		}
	}
	void JobSystem::Shutdown()
	{
		if (!m_Running) return;

		// 1. 发出停止信号
		m_Running = false;

		// 2. 唤醒所有正在 sleep 的线程（如果使用了 CV 机制）
		// 在目前的 sleep_for 实现中，线程会在下一次轮询时检测到 m_running 为 false

		// 3. 等待所有工作线程安全退出
		for (std::thread& worker : m_Workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}

		// 4. 清理资源
		if (m_Queues)
		{
			delete[] m_Queues;
			m_Queues = nullptr;
		}

		m_Workers.clear();

		// 归位
		m_NumWorkers = 0;
		LocalIndex() = 0;

		// 💡 工业级提示：可以在这里加一行日志，确认系统已安全关闭
		WLD_CORE_INFO("JobSystem shutdown successfully.");
	}
	bool JobSystem::GetNextJob(JobDecl& outJob)
	{
		// 先尝试从本线程队列拿
		if (m_Queues[LocalIndex()].Pop(outJob)) return true;

		// 拿不到就去偷别人的
		for (uint32_t i = 0; i <= m_NumWorkers; ++i)
		{
			if (i == LocalIndex()) continue;
			if (m_Queues[i].Steal(outJob)) return true;
		}
		return false;
	}
	void JobSystem::WorkerLoop()
	{
		uint32_t idleTime = 0; // 记录线程空闲（拿不到任务）的次数
		while (m_Running)
		{
			JobDecl job;
			if (GetNextJob(job))
			{
				idleTime = 0;
				// 将包裹负载数据的那个内部连续内存地址传进去
				if (job.Entry)
					job.Entry(job.Padding);

				if (job.Counter)
				{
					// 这里必须用 memory_order_release，确保前面活儿的数据都写进内存了
					job.Counter->Count.fetch_sub(1, std::memory_order_release);
				}
			}
			else
			{
				// 没拿到任务，开始梯度退避策略
				idleTime++;
				if (idleTime < 10)
				{
					// 第一阶段（刚闲下来）：只使用轻微的硬件暂停指令，提示 CPU 这是一个自旋锁循环
					// 这避免了系统级的线程切换开销，响应最快
					#if defined(_MSC_VER)
					_mm_pause(); // 需要包含 <immintrin.h> 或使用平台特定的微架构暂停指令
					#endif
				}
				else if (idleTime < 100)
				{
					// 第二阶段（闲了一小会）：出让时间片，让系统调度其他线程，但保持活跃状态
					std::this_thread::yield();
				}
				else
				{
					// 第三阶段（长期处于彻底无任务状态）：强制休眠一小段时间，大幅降低 CPU 占用（节能降温）
					std::this_thread::sleep_for(std::chrono::microseconds(100)); // 注意：在Windows下实际可能偏长
				}
			}
		}
	}
}
