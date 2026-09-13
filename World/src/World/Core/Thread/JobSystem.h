#pragma once
#include "Thread.h"
#include "World/Core/Export.h"
#include "World/Core/Memory/StackAllocator.h"
namespace World
{
	class JobQueue;
	struct JobDecl;

	class JobSystem
	{
	public:

		static void Init();

		// 提交一个任务包
		static void Kick(JobDecl job);

		// 核心：非阻塞等待（边等边干活）
		static void Wait(JobCounter* counter);

		static void Shutdown();
	private:
		// 获取下一个可执行的任务，优先从本地队列拿，拿不到就去偷别人的
		static bool GetNextJob(JobDecl& outJob);

		// 工作线程主循环
		static void WorkerLoop();

	private:
		static uint32_t& LocalIndex(); // 线程局部索引，主线程为最后一个
		static WLD_API std::vector<std::thread> m_Workers; // 工作线程列表
		static WLD_API JobQueue* m_Queues; // 作业队列
		static WLD_API uint32_t m_NumWorkers; // 工作线程数量
		static WLD_API std::atomic<bool> m_Running; // 作业系统运行状态

	public:
		/**
		 * @brief 工业级并行循环
		 * @param count 迭代总数
		 * @param batchSize 每个 Job 处理的数量
		 * @param func 执行逻辑 (Lambda)
		 */
		template<typename F>
		static void ParallelFor(uint32_t count, uint32_t batchSize, F&& func)
		{
			if (count == 0 || batchSize == 0) return;
			struct ParallelForData
			{
				F func;
				uint32_t start;// 起始索引
				uint32_t end;// 结束索引（不包含）

				ParallelForData(const F& f, uint32_t s, uint32_t e)
					: func(f), start(s), end(e)
				{}
			};

			JobCounter sync;
			for (uint32_t i = 0; i < count; i += batchSize)
			{
				uint32_t end = std::min(i + batchSize, count);

				JobDecl job;

				// 将执行逻辑和状态数据强行塞入预留的 64 字节缝隙中！0 内存分配！
				job.Emplace(ParallelForData(func, i, end));

				job.Entry = [](void* voidData)
					{
						// 从那块预留缝隙中还原为我们实际的结构体
						auto* pData = static_cast<ParallelForData*>(voidData);
						for (uint32_t j = pData->start; j < pData->end; ++j)
						{
							pData->func(j);
						}

					};
				job.Counter = &sync;

				// 把这个自己内包揽了状态的肥胖 Job结构（通常不到80字节）直接推进无锁队列数组
				Kick(job);
			}

			Wait(&sync);
		}
	};
}
