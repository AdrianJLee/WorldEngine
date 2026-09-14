#include "World/Core/Thread/JobSystem.h"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	// 通用任务负载:累加数值并计数(可安全内联存放)。
	struct AccumulateJob
	{
		std::atomic<int>* Executed = nullptr;
		std::atomic<long long>* Sum = nullptr;
		int Value = 0;

		void operator()() const
		{
			Executed->fetch_add(1, std::memory_order_relaxed);
			Sum->fetch_add(Value, std::memory_order_relaxed);
		}
	};

}

int main()
{
	try
	{
		World::JobSystem::Init();
		CHECK(World::JobSystem::IsRunning());
		CHECK(World::JobSystem::WorkerCount() >= 1);

		// 1. 基本提交/等待:任务全部执行且结果正确
		{
			constexpr int kJobs = 4096;
			World::JobCounter counter;
			std::atomic<int> executed { 0 };
			std::atomic<long long> sum { 0 };
			for (int i = 1; i <= kJobs; ++i)
			{
				AccumulateJob payload { &executed, &sum, i };
				World::JobDecl job;
				job.Emplace(payload);
				job.Entry = [](void* data) { (*static_cast<AccumulateJob*>(data))(); };
				job.Counter = &counter;
				World::JobSystem::Kick(std::move(job));
			}
			World::JobSystem::Wait(&counter);
			CHECK(executed.load() == kJobs);
			const long long expected = static_cast<long long>(kJobs) * (kJobs + 1) / 2;
			CHECK(sum.load() == expected);
		}

		// 2. ParallelFor:分批并行 + 结果正确
		{
			constexpr uint32_t kCount = 1u << 20;
			std::atomic<long long> sum { 0 };
			World::JobSystem::ParallelFor(kCount, 4096, [&sum](uint32_t index)
			{
				sum.fetch_add(index, std::memory_order_relaxed);
			});
			const long long expected = static_cast<long long>(kCount) * (kCount - 1) / 2;
			CHECK(sum.load() == expected);
		}

		// 3. 嵌套依赖:任务内部再提交并等待(job 内部 Wait 不应死锁)
		{
			World::JobCounter outerCounter;
			World::JobCounter innerCounter;
			std::atomic<int> innerExecuted { 0 };
			std::atomic<long long> innerSum { 0 };

			struct OuterJob
			{
				World::JobCounter* Inner;
				std::atomic<int>* InnerExecuted;
				std::atomic<long long>* InnerSum;
			};
			OuterJob outer { &innerCounter, &innerExecuted, &innerSum };

			World::JobDecl job;
			job.Emplace(outer);
			job.Entry = [](void* data)
			{
				auto* payload = static_cast<OuterJob*>(data);
				constexpr int kInnerJobs = 64;
				for (int i = 1; i <= kInnerJobs; ++i)
				{
					AccumulateJob innerPayload { payload->InnerExecuted, payload->InnerSum, i };
					World::JobDecl innerJob;
					innerJob.Emplace(innerPayload);
					innerJob.Entry = [](void* innerData) { (*static_cast<AccumulateJob*>(innerData))(); };
					innerJob.Counter = payload->Inner;
					World::JobSystem::Kick(std::move(innerJob));
				}
				World::JobSystem::Wait(payload->Inner);
			};
			job.Counter = &outerCounter;
			World::JobSystem::Kick(std::move(job));
			World::JobSystem::Wait(&outerCounter);
			CHECK(innerExecuted.load() == 64);
			CHECK(innerSum.load() == 64 * 65 / 2);
		}

		// 4. 取消:已取消计数器上的任务被跳过,Wait 仍然返回(不会死锁)
		{
			constexpr int kJobs = 4096;
			World::JobCounter counter;
			counter.Cancel();   // 提交前即取消:所有任务都必须被跳过(确定性)
			std::atomic<int> executed { 0 };
			std::atomic<long long> sum { 0 };
			for (int i = 1; i <= kJobs; ++i)
			{
				AccumulateJob payload { &executed, &sum, i };
				World::JobDecl job;
				job.Emplace(payload);
				job.Entry = [](void* data) { (*static_cast<AccumulateJob*>(data))(); };
				job.Counter = &counter;
				World::JobSystem::Kick(std::move(job));
			}
			World::JobSystem::Wait(&counter);
			CHECK(counter.IsComplete());
			CHECK(executed.load() == 0);
			CHECK(sum.load() == 0);
		}

		// 5. 压力:大量小任务 + 混合优先级(顺便覆盖溢出队列)
		{
			constexpr int kJobs = 200000;
			World::JobCounter counter;
			std::atomic<int> executed { 0 };
			std::atomic<long long> sum { 0 };
			for (int i = 0; i < kJobs; ++i)
			{
				AccumulateJob payload { &executed, &sum, 1 };
				World::JobDecl job;
				job.Emplace(payload);
				job.Entry = [](void* data) { (*static_cast<AccumulateJob*>(data))(); };
				job.Counter = &counter;
				const World::JobPriority priority = (i % 3 == 0) ? World::JobPriority::High
					: (i % 3 == 1 ? World::JobPriority::Normal : World::JobPriority::Low);
				World::JobSystem::Kick(std::move(job), priority);
			}
			World::JobSystem::Wait(&counter);
			CHECK(executed.load() == kJobs);
			CHECK(sum.load() == kJobs);
		}

		// 6. 统计信息可用
		{
			const World::JobSystem::Stats stats = World::JobSystem::GetStats();
			CHECK(stats.Executed > 0);
			CHECK(stats.Waits >= 4);
			CHECK(!World::JobSystem::DescribeStats().empty());
		}

		World::JobSystem::Shutdown();
		CHECK(!World::JobSystem::IsRunning());

		// 7. 关闭后提交:同步执行(不能丢任务)
		{
			std::atomic<int> executed { 0 };
			std::atomic<long long> sum { 0 };
			AccumulateJob payload { &executed, &sum, 42 };
			World::JobDecl job;
			job.Emplace(payload);
			job.Entry = [](void* data) { (*static_cast<AccumulateJob*>(data))(); };
			World::JobSystem::Kick(std::move(job));
			CHECK(executed.load() == 1);
			CHECK(sum.load() == 42);
		}

		std::printf("World.Tasks: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Tasks: FAILED: %s\n", error.what());
		return 1;
	}
}


