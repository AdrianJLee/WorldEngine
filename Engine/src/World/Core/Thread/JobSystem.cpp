#include "wldpch.h"
#include "JobSystem.h"

#include "JobQueue.h"

#include <algorithm>
#include <cstdlib>
#include <chrono>

namespace World
{
	namespace
	{
		constexpr uint32_t kPriorityCount = static_cast<uint32_t>(JobPriority::Count);
		constexpr uint32_t kMaxWorkers = 64;

		std::vector<std::thread> s_Workers;
		JobQueue* s_Queues = nullptr;         // [queueCount][kPriorityCount]
		uint32_t s_WorkerCount = 0;
		std::atomic<bool> s_Running { false };
		std::atomic<int> s_PendingJobs { 0 };  // 已提交未完成(含未开始)的任务数

		// 溢出队列:某条线程队列满时接管,保证任务不丢。
		std::mutex s_OverflowMutex;
		std::deque<JobDecl> s_Overflow;

		// 等待/唤醒:提交与完成时通知阻塞中的线程。
		std::mutex s_WakeMutex;
		std::condition_variable s_WakeCv;
		std::atomic<uint32_t> s_WaiterCount { 0 };   // 无等待者时跳过锁与 notify(提交路径上的大头开销)

		std::atomic<uint64_t> s_StatExecuted { 0 };
		std::atomic<uint64_t> s_StatStolen { 0 };
		std::atomic<uint64_t> s_StatExecutedWhileWaiting { 0 };
		std::atomic<uint64_t> s_StatOverflowPushes { 0 };
		std::atomic<uint64_t> s_StatQueueHighWater { 0 };
		std::atomic<uint64_t> s_StatWaits { 0 };
	}

	uint32_t& JobSystem::LocalIndex()
	{
		static thread_local uint32_t index = 0;
		return index;
	}

	uint32_t JobSystem::QueueCount()
	{
		return s_WorkerCount + 1;   // 工作线程 + 主线程
	}

	bool JobSystem::IsRunning()
	{
		return s_Running.load(std::memory_order_acquire);
	}

	uint32_t JobSystem::WorkerCount()
	{
		return s_WorkerCount;
	}

	void JobSystem::Init(uint32_t threadCount)
	{
		if (s_Running.load(std::memory_order_acquire))
			return;

		if (threadCount == 0)
		{
			if (const char* fromEnvironment = std::getenv("WLD_JOB_THREADS"))
			{
				const int parsed = std::atoi(fromEnvironment);
				if (parsed > 0)
					threadCount = static_cast<uint32_t>(parsed);
			}
		}
		if (threadCount == 0)
		{
			const uint32_t hardware = std::thread::hardware_concurrency();
			threadCount = hardware > 1 ? hardware - 1 : 1;   // 修掉 hc-1 在 hc==0 时的下溢
		}
		threadCount = std::min(threadCount, kMaxWorkers);

		s_WorkerCount = threadCount;
		s_Queues = new JobQueue[QueueCount() * kPriorityCount];
		s_Running.store(true, std::memory_order_release);
		s_PendingJobs.store(0, std::memory_order_relaxed);

		LocalIndex() = s_WorkerCount;   // 主线程使用最后一条队列

		for (uint32_t i = 0; i < s_WorkerCount; ++i)
		{
			s_Workers.emplace_back([i]()
			{
				LocalIndex() = i;
				WorkerLoop();
			});
		}

		WLD_CORE_INFO("JobSystem initialized with {0} worker thread(s)", s_WorkerCount);
	}

	void JobSystem::Shutdown()
	{
		if (!s_Running.exchange(false, std::memory_order_acq_rel))
			return;

		NotifyAll();
		for (std::thread& worker : s_Workers)
			if (worker.joinable())
				worker.join();
		s_Workers.clear();

		{
			std::lock_guard<std::mutex> lock(s_OverflowMutex);
			for (JobDecl& job : s_Overflow)
				Complete(job);   // 未执行的任务也要归还计数器/负载
			s_Overflow.clear();
		}

		delete[] s_Queues;
		s_Queues = nullptr;
		s_WorkerCount = 0;
		LocalIndex() = 0;
		WLD_CORE_INFO("JobSystem shutdown successfully.");
	}

	void JobSystem::Kick(JobDecl job, JobPriority priority)
	{
		if (!s_Running.load(std::memory_order_acquire))
		{
			// 未初始化(或已关闭):同步执行,保持调用方语义。
			JobDecl local = std::move(job);
			if (local.Counter)
				local.Counter->Count.fetch_add(1, std::memory_order_release);
			Execute(local, false);
			local.Release();
			if (local.Counter)
			{
				local.Counter->Count.fetch_sub(1, std::memory_order_acq_rel);
				local.Counter = nullptr;
			}
			return;
		}

		job.Priority = priority;
		if (job.Counter)
			job.Counter->Count.fetch_add(1, std::memory_order_release);
		s_PendingJobs.fetch_add(1, std::memory_order_acq_rel);

		const uint32_t queueIndex = LocalIndex() * kPriorityCount + static_cast<uint32_t>(priority);
		if (!s_Queues[queueIndex].Push(job))
			PushOverflow(job);

		const uint64_t queued = static_cast<uint64_t>(s_PendingJobs.load(std::memory_order_relaxed));
		uint64_t high = s_StatQueueHighWater.load(std::memory_order_relaxed);
		while (queued > high && !s_StatQueueHighWater.compare_exchange_weak(high, queued, std::memory_order_relaxed))
		{
		}

		NotifyAll();
	}

	void JobSystem::PushOverflow(JobDecl& job)
	{
		std::lock_guard<std::mutex> lock(s_OverflowMutex);
		JobDecl moved = std::move(job);
		s_Overflow.push_back(std::move(moved));
		s_StatOverflowPushes.fetch_add(1, std::memory_order_relaxed);
	}

	bool JobSystem::TryGetOverflow(JobDecl& outJob)
	{
		std::lock_guard<std::mutex> lock(s_OverflowMutex);
		if (s_Overflow.empty())
			return false;
		outJob = std::move(s_Overflow.front());
		s_Overflow.pop_front();
		return true;
	}

	bool JobSystem::PopLocal(JobDecl& outJob)
	{
		const uint32_t base = LocalIndex() * kPriorityCount;
		for (uint32_t priority = kPriorityCount; priority-- > 0;)
			if (s_Queues[base + priority].Pop(outJob))
				return true;
		return false;
	}

	bool JobSystem::TryGetJob(JobDecl& outJob, bool& stolen)
	{
		stolen = false;
		if (PopLocal(outJob))
			return true;
		if (TryGetOverflow(outJob))
			return true;
		for (uint32_t priority = kPriorityCount; priority-- > 0;)
		{
			for (uint32_t i = 0; i < QueueCount(); ++i)
			{
				if (i == LocalIndex())
					continue;
				if (s_Queues[i * kPriorityCount + priority].Steal(outJob))
				{
					stolen = true;
					return true;
				}
			}
		}
		return false;
	}

	void JobSystem::Execute(JobDecl& job, bool whileWaiting)
	{
		const bool cancelled = job.Counter && job.Counter->IsCancelled();
		if (job.Entry && !cancelled)
		{
			job.Entry(job.Data());
			s_StatExecuted.fetch_add(1, std::memory_order_relaxed);
			if (whileWaiting)
				s_StatExecutedWhileWaiting.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void JobSystem::Complete(JobDecl& job)
	{
		job.Release();
		if (job.Counter)
		{
			job.Counter->Count.fetch_sub(1, std::memory_order_acq_rel);
			job.Counter = nullptr;
		}
		s_PendingJobs.fetch_sub(1, std::memory_order_acq_rel);
		NotifyAll();
	}

	void JobSystem::NotifyAll()
	{
		if (s_WaiterCount.load(std::memory_order_relaxed) == 0)
			return;   // 没有线程在等:不取锁、不 notify
		std::lock_guard<std::mutex> lock(s_WakeMutex);
		s_WakeCv.notify_all();
	}

	void JobSystem::Wait(JobCounter* counter)
	{
		if (!counter)
			return;
		s_StatWaits.fetch_add(1, std::memory_order_relaxed);

		uint32_t spin = 0;
		while (!counter->IsComplete())
		{
			JobDecl job;
			bool stolen = false;
			if (TryGetJob(job, stolen))
			{
				if (stolen)
					s_StatStolen.fetch_add(1, std::memory_order_relaxed);
				Execute(job, true);
				Complete(job);
				continue;
			}

			if (spin++ < 64)
			{
				std::this_thread::yield();
				continue;
			}

			std::unique_lock<std::mutex> lock(s_WakeMutex);
			s_WaiterCount.fetch_add(1, std::memory_order_relaxed);
			s_WakeCv.wait_for(lock, std::chrono::milliseconds(1), [&counter]()
			{
				return counter->IsComplete();
			});
			s_WaiterCount.fetch_sub(1, std::memory_order_relaxed);
		}
	}

	void JobSystem::WaitAll()
	{
		uint32_t spin = 0;
		while (s_PendingJobs.load(std::memory_order_acquire) > 0)
		{
			JobDecl job;
			bool stolen = false;
			if (TryGetJob(job, stolen))
			{
				if (stolen)
					s_StatStolen.fetch_add(1, std::memory_order_relaxed);
				Execute(job, true);
				Complete(job);
				continue;
			}
			if (spin++ < 64)
			{
				std::this_thread::yield();
				continue;
			}
			std::unique_lock<std::mutex> lock(s_WakeMutex);
			s_WaiterCount.fetch_add(1, std::memory_order_relaxed);
			s_WakeCv.wait_for(lock, std::chrono::milliseconds(1));
			s_WaiterCount.fetch_sub(1, std::memory_order_relaxed);
		}
	}

	void JobSystem::WorkerLoop()
	{
		uint32_t spin = 0;
		while (s_Running.load(std::memory_order_acquire))
		{
			JobDecl job;
			bool stolen = false;
			if (TryGetJob(job, stolen))
			{
				spin = 0;
				if (stolen)
					s_StatStolen.fetch_add(1, std::memory_order_relaxed);
				Execute(job, false);
				Complete(job);
				continue;
			}

			// 空闲:短自旋(低延迟) → 条件变量阻塞(不烧 CPU)。
			if (spin++ < 256)
			{
				std::this_thread::yield();
				continue;
			}

			std::unique_lock<std::mutex> lock(s_WakeMutex);
			s_WaiterCount.fetch_add(1, std::memory_order_relaxed);
			s_WakeCv.wait_for(lock, std::chrono::milliseconds(2));
			s_WaiterCount.fetch_sub(1, std::memory_order_relaxed);
		}
	}

	JobSystem::Stats JobSystem::GetStats()
	{
		Stats stats;
		stats.Executed = s_StatExecuted.load(std::memory_order_relaxed);
		stats.Stolen = s_StatStolen.load(std::memory_order_relaxed);
		stats.ExecutedWhileWaiting = s_StatExecutedWhileWaiting.load(std::memory_order_relaxed);
		stats.OverflowPushes = s_StatOverflowPushes.load(std::memory_order_relaxed);
		stats.QueueHighWater = s_StatQueueHighWater.load(std::memory_order_relaxed);
		stats.Waits = s_StatWaits.load(std::memory_order_relaxed);
		return stats;
	}

	std::string JobSystem::DescribeStats()
	{
		const Stats stats = GetStats();
		return "workers=" + std::to_string(s_WorkerCount) +
			" executed=" + std::to_string(stats.Executed) +
			" stolen=" + std::to_string(stats.Stolen) +
			" helped=" + std::to_string(stats.ExecutedWhileWaiting) +
			" overflow=" + std::to_string(stats.OverflowPushes) +
			" highWater=" + std::to_string(stats.QueueHighWater) +
			" waits=" + std::to_string(stats.Waits);
	}
}

