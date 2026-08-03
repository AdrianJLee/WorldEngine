#include "wldpch.h"
#include "JobQueue.h"
#include "Thread.h"

namespace World
{
	JobQueue::JobQueue()
	{
		m_Jobs = new JobDecl[CAPACITY];
	}
	JobQueue::~JobQueue()
	{
		delete[] m_Jobs;
	}
	void JobQueue::Push(JobDecl job)
	{
		// 由于只有生产者线程会调用 Push，消费者线程会调用 Pop 和 Steal，因此我们可以使用无锁的方式来实现这个队列

		// 获取当前的底部索引，使用 relaxed 内存顺序，因为我们不需要在这个阶段保证任何特定的顺序
		int64_t b = m_Bottom.load(std::memory_order_relaxed);
		int64_t t = m_Top.load(std::memory_order_acquire);


		if (b - t >= CAPACITY)
		{
			// 队列满了！
			// 工业级策略：不再入队，而是由当前线程直接执行该任务（立刻消化掉）
			job.Entry(job.Padding);
			if (job.Counter)
			{
				job.Counter->Count.fetch_sub(1, std::memory_order_release);
			}

			return;
		}


		// 范围检查，确保不会覆盖未被消费的任务
		m_Jobs[b & (CAPACITY - 1)] = job;

		// 发布内存屏障，确保在更新 m_Bottom 之前，所有对 m_Jobs 的写入都对其他线程可见
		std::atomic_thread_fence(std::memory_order_release);

		// 更新底部索引，使用 relaxed 内存顺序，因为我们已经通过内存屏障确保了可见性
		m_Bottom.store(b + 1, std::memory_order_relaxed);
	}
	bool JobQueue::Pop(JobDecl& outJob)
	{
		// 获取最新的底部索引，使用 relaxed 内存顺序，因为我们不需要在这个阶段保证任何特定的顺序
		int64_t b = m_Bottom.load(std::memory_order_relaxed) - 1;

		// 更新底部索引，使用 relaxed 内存顺序，因为我们稍后会通过内存屏障来确保可见性
		m_Bottom.store(b, std::memory_order_relaxed);

		// 发布内存屏障，确保在读取 m_Top 之前，所有对 m_Bottom 的写入都对其他线程可见
		std::atomic_thread_fence(std::memory_order_seq_cst);

		int64_t t = m_Top.load(std::memory_order_relaxed);

		if (t <= b)
		{
			outJob = m_Jobs[b & (CAPACITY - 1)];

			if (t == b)
			{
				// 只有一个任务时,检测是否被窃取了
				// 如果还是t，说明没有被窃取，成功弹出；如果不是t，说明被窃取了，弹出失败
				if (m_Top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
				{
					// t变为t+1，防止其他线程继续窃取，弹出成功
					// 队列为空时，底部索引和顶部索引相等
					m_Bottom.store(b + 1, std::memory_order_relaxed);
					return true;
				}
				else
				{
					// 被窃取了，弹出失败
					// 被窃取时,Top会+1,所以这里恢复Bottom时要+1
					m_Bottom.store(b + 1, std::memory_order_relaxed);
					return false;
				}

			}
			else
			{
				return true;
			}

		}
		// 范围检查失败，恢复底部索引
		m_Bottom.store(b + 1, std::memory_order_relaxed);

		return false;
	}
	bool JobQueue::Steal(JobDecl& outJob)
	{
		int64_t t = m_Top.load(std::memory_order_acquire);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		int64_t b = m_Bottom.load(std::memory_order_acquire);

		if (t < b)
		{
			outJob = m_Jobs[t & (CAPACITY - 1)];
			if (m_Top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
			{
				return true;
			}
			else
			{
				return false;
			}

		}
		return false;
	}
}