#pragma once
#include "Thread.h"
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
		static void Kick(JobDecl job, JobCounter* counter = nullptr);

		// 核心：非阻塞等待（边等边干活）
		static void Wait(JobCounter* counter);

		static void Shutdown();
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
		static inline std::atomic<bool> m_Running { false }; // 作业系统运行状态

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
			struct ParallelForData
			{
				F func;
				uint32_t start;// 起始索引
				uint32_t end;// 结束索引（不包含）
			};


			// 预估大小：(任务数) * (数据包大小)
			uint32_t numJobs = (count + batchSize - 1) / batchSize;
			WLD_STACK_WIZARD(tempPayloads, numJobs * 128);

			JobCounter sync;
			for (uint32_t i = 0; i < count; i += batchSize)
			{
				uint32_t end = std::min(i + batchSize, count);

				// 包装 Lambda 和范围数据
				// 在实际工业实现中，我们会把 Lambda 对象拷贝到 Job 的 Data 内存里
				auto* data = WLD_STACK_NEW(ParallelForData, tempPayloads, std::forward<F>(func), i, end);
				auto wrapper = [](void* voidData)
					{
						auto* pData = static_cast<ParallelForData*>(voidData);
						for (uint32_t j = pData->start; j < pData->end; ++j)
						{
							pData->func(j);
						}
					};

				Kick({ wrapper, data }, &sync);
			}

			Wait(&sync);
		}
	};
}
