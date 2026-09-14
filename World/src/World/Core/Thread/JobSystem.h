#pragma once

#include "Thread.h"

#include "World/Core/Export.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace World
{
	class JobQueue;

	// 工作窃取任务系统(工业级):
	// - 每线程一条队列 + 三级优先级(Low/Normal/High);
	// - 队列满时进入全局溢出队列(不丢任务、不在提交线程内联执行);
	// - 等待用"帮忙执行 + 条件变量阻塞",不再忙等 yield;
	// - 计数器取消(Cancel)会跳过尚未开始的任务;
	// - 内置统计:执行数/窃取数/等待期执行数/溢出次数/队列峰值。
	class JobSystem
	{
	public:
		// threadCount = 0 时:取 WLD_JOB_THREADS 环境变量,否则 max(1, hardware_concurrency-1)。
		static void Init(uint32_t threadCount = 0);
		static void Shutdown();
		static bool IsRunning();

		// 提交任务(默认 Normal 优先级)。Counter 非空时会在提交处自增、完成后自减。
		static void Kick(JobDecl job, JobPriority priority = JobPriority::Normal);

		// 等待计数器归零:先自旋/帮忙执行,随后按条件变量阻塞(不空转 CPU)。
		static void Wait(JobCounter* counter);
		// 等待系统内所有已提交任务完成(测试/关卡切换用)。
		static void WaitAll();

		// 并行 for:count 个索引按 batchSize 分批执行;内部使用计数器,调用线程也参与执行。
		template <typename F>
		static void ParallelFor(uint32_t count, uint32_t batchSize, F&& func)
		{
			if (count == 0 || batchSize == 0)
				return;
			struct Range
			{
				F Func;
				uint32_t Begin = 0;
				uint32_t End = 0;
			};

			JobCounter counter;
			for (uint32_t begin = 0; begin < count; begin += batchSize)
			{
				const uint32_t end = (begin + batchSize < count) ? begin + batchSize : count;
				JobDecl job;
				Range range { func, begin, end };
				job.Emplace(std::move(range));
				job.Entry = [](void* data)
				{
					auto* range = static_cast<Range*>(data);
					for (uint32_t i = range->Begin; i < range->End; ++i)
						range->Func(i);
				};
				job.Priority = JobPriority::Normal;
				job.Counter = &counter;
				Kick(std::move(job));
			}
			Wait(&counter);
		}

		static uint32_t WorkerCount();

		struct Stats
		{
			uint64_t Executed = 0;          // 总共执行的任务数
			uint64_t Stolen = 0;            // 被其它线程偷走执行的任务数
			uint64_t ExecutedWhileWaiting = 0; // 等待期间"帮忙"执行的任务数
			uint64_t OverflowPushes = 0;    // 进入溢出队列的次数
			uint64_t QueueHighWater = 0;    // 队列长度峰值
			uint64_t Waits = 0;             // Wait 调用次数
		};
		static Stats GetStats();
		static std::string DescribeStats();

	private:
		static bool PopLocal(JobDecl& outJob);
		static bool TryGetJob(JobDecl& outJob, bool& stolen);
		static bool TryGetOverflow(JobDecl& outJob);
		static void Execute(JobDecl& job, bool whileWaiting);
		static void Complete(JobDecl& job);
		static void PushOverflow(JobDecl& job);
		static void NotifyAll();
		static void WorkerLoop();
		static uint32_t& LocalIndex();
		static uint32_t QueueCount();   // worker 数 + 1(主线程)
	};
}
