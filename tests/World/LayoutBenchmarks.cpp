// 数据布局基准(标准 docs/dev/performance-and-data-layout.md §4.1 Q1 / §6 O0)。
//
// **手动目标,不进 ctest**:基准数字依赖机器与负载,不是每次提交都必须绿的门禁。
// 跑法(主 agent / we_verifier 在指定构建目录里独占执行):
//   cmake --build build/x64-Debug --config Debug --target WorldLayoutBenchmarks
//   build/x64-Debug/tests/Debug/WorldLayoutBenchmarks.exe
//
// 它回答标准 §4.1 Q1"热吗?"这个问题,让 O1/O6 这类优化**可证伪**:
//   用例 A(伪共享):多线程反复 Kick/Complete,量 JobSystem 提交路径吞吐。
//                    F7 的结构性理由(六个热统计原子 + s_PendingJobs 同线)在此量出代价。
//   用例 B(步长):顺序遍历 48B 组件(AoS,现状)vs 64B 补齐到整条 cache line 的等价结构。
//                  结论允许是"维持 48B" —— 标准明确禁止无数字的预防性对齐。
//   用例 C(随机访存):同一批数据按随机下标取元素(模拟单实体访问),
//                   与顺序遍历对照,量"跨线"在非流式访问下是否真的吃亏。
//
// 每用例都带 checksum 并打印它,防止编译器把循环优化掉(不靠 volatile/asm)。
#include "World/Core/Thread/JobSystem.h"
#include "World/Scene/Components.h"
#include "World/Scene/Systems/TransformSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

namespace
{
	using namespace World;
	using Clock = std::chrono::steady_clock;

	double Millis(Clock::time_point a, Clock::time_point b)
	{
		return std::chrono::duration<double, std::milli>(b - a).count();
	}

	volatile uint64_t g_Sink = 0;

	// 用例 A:伪共享探针。
	// 8 线程各提交并等待自己的一批任务;总核数固定,唯一变量是 JobSystem 内部
	// "热计数原子是否独占 cache line"。它与 F7 的改动一一对应(改前/改后各跑一次)。
	// 单提交线程 + N 工作线程并发完成:量的是**同一批全局原子**上的竞争
	// (提交侧 s_PendingJobs++ ,完成侧 s_StatExecuted++ / s_PendingJobs--)。
	// 这正是 F7 指出的伪共享路径:改前全部挤在一条 cache line,改后各自独占。
	// 只用主线程提交:JobSystem 的 LocalIndex() 是 thread_local,外部临时线程没有队列槽位,
	// 拿它当提交者会测到"所有线程挤同一队列"的另一类竞争,不是本用例要量的东西。
	void CaseA_JobSubmitThroughput(uint32_t taskCount, uint32_t workerCount)
	{
		JobSystem::Init(workerCount);

		// 基准自身也必须守 R3:这个计数器被所有 worker 并发写,所以用 atomic 并独占占位,
		// 不能用裸 ++ —— 否则量到的是基准自己的伪共享,不是 JobSystem 的。
		struct Payload { std::atomic<uint64_t>* Out; };
		alignas(64) std::atomic<uint64_t> completed { 0 };
		JobCounter counter;

		const auto begin = Clock::now();
		for (uint32_t i = 0; i < taskCount; ++i)
		{
			JobDecl job;
			job.Emplace(Payload { &completed });
			job.Entry = [](void* data) { static_cast<Payload*>(data)->Out->fetch_add(1, std::memory_order_relaxed); };
			job.Priority = JobPriority::Normal;
			job.Counter = &counter;
			JobSystem::Kick(std::move(job));
		}
		JobSystem::Wait(&counter);
		const auto end = Clock::now();

		const JobSystem::Stats stats = JobSystem::GetStats();
		const double ms = Millis(begin, end);
		std::printf("A. job Kick+Wait   : %8.2f ms total | %8.1f ns/task | executed=%llu stolen=%llu helped=%llu | completed=%llu\n",
			ms, ms * 1.0e6 / static_cast<double>(taskCount),
			static_cast<unsigned long long>(stats.Executed),
			static_cast<unsigned long long>(stats.Stolen),
			static_cast<unsigned long long>(stats.ExecutedWhileWaiting),
			static_cast<unsigned long long>(completed.load(std::memory_order_relaxed)));

		JobSystem::Shutdown();
	}

	// 48B 现状(真身,不是镜像:用真组件才不会与实现漂移)
	using Packed48 = TransformComponent;
	static_assert(sizeof(Packed48) == 48, "TransformComponent must be the 48B baseline");

	// 补齐到 64B 的对照体:同样字段 + 尾部填充(标准 §4.8 的反模式,只在有数字时可用)。
	// 用 alignas(64) 让每个元素从线边界开始 —— 这是"顺带也拿到对齐"的乐观版本,
	// 即让 64B 方案处于最有利条件;若它仍不赢,就没有理由做。
	struct alignas(64) Padded64
	{
		TransformComponent T;
		uint8_t Padding[64 - sizeof(TransformComponent)];
	};
	static_assert(sizeof(Padded64) == 64, "Padded64 must be exactly one cache line");

	template <typename T>
	void RunSequential(const char* label, std::size_t count, uint32_t repeats)
	{
		std::vector<T> data(count);
		for (std::size_t i = 0; i < count; ++i)
			reinterpret_cast<float*>(&data[i])[0] = static_cast<float>(i);

		const auto begin = Clock::now();
		float acc = 0.0f;
		for (uint32_t r = 0; r < repeats; ++r)
			for (std::size_t i = 0; i < count; ++i)
				acc += reinterpret_cast<float*>(&data[i])[0];
		const auto end = Clock::now();

		const double ms = Millis(begin, end);
		const double elements = static_cast<double>(count) * repeats;
		std::printf("B. seq %-10s : %8.2f ms | %8.3f ns/elem | %.3f lines/elem (stride %zuB)\n",
			label, ms, ms * 1.0e6 / elements,
			static_cast<double>(sizeof(T)) / 64.0, sizeof(T));
		g_Sink += static_cast<uint64_t>(acc);
	}

	template <typename T>
	void RunRandom(const char* label, std::size_t count, uint32_t repeats, uint32_t seed)
	{
		std::vector<T> data(count);
		for (std::size_t i = 0; i < count; ++i)
			reinterpret_cast<float*>(&data[i])[0] = static_cast<float>(i);

		std::vector<uint32_t> order(count);
		std::iota(order.begin(), order.end(), 0u);
		std::mt19937 rng(seed);
		std::shuffle(order.begin(), order.end(), rng);

		const auto begin = Clock::now();
		float acc = 0.0f;
		for (uint32_t r = 0; r < repeats; ++r)
			for (std::size_t i = 0; i < count; ++i)
				acc += reinterpret_cast<float*>(&data[order[i]])[0];
		const auto end = Clock::now();

		const double ms = Millis(begin, end);
		const double elements = static_cast<double>(count) * repeats;
		std::printf("C. rnd %-10s : %8.2f ms | %8.3f ns/elem | %.3f lines/elem (stride %zuB)\n",
			label, ms, ms * 1.0e6 / elements,
			static_cast<double>(sizeof(T)) / 64.0, sizeof(T));
		g_Sink += static_cast<uint64_t>(acc);
	}

	// 用例 D:热字段跨线检查(标准 §4.8 M1)。
	// 列出组件里"每帧读写"字段的偏移与所在 cache line,以及是否跨越线边界。
	void CaseD_HotFieldCrossLine()
	{
		std::printf("D. hot-field cache lines (offset / line / crosses?):\n");
		auto report = [](const char* component, const char* field, std::size_t offset, std::size_t size)
		{
			const std::size_t line = offset / 64;
			const bool crosses = ((offset % 64) + size) > 64;
			std::printf("   %-28s %-18s off=%4zu line=%zu %s\n",
				component, field, offset, line, crosses ? "CROSSES LINE" : "ok");
		};
		report("World::TransformComponent", "Location", offsetof(TransformComponent, Location), sizeof(glm::vec3));
		report("World::TransformComponent", "Scale", offsetof(TransformComponent, Scale), sizeof(glm::vec3));
		report("World::MeshRendererComponent", "Color", offsetof(MeshRendererComponent, Color), sizeof(glm::vec4));
		report("World::SpriteComponent", "Color", offsetof(SpriteComponent, Color), sizeof(glm::vec4));
		report("World::VelocityComponent", "Linear", offsetof(VelocityComponent, Linear), sizeof(glm::vec3));
	}
}

int main(int argc, char** argv)
{
	const uint32_t taskCount = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 60000u;
	const uint32_t workers = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 4u;
	const std::size_t count = 1u << 18;   // 262144 元素 ≈ 12.6MB(48B)→ 16MB(64B)
	const uint32_t repeats = 8u;

	std::printf("WorldEngine layout benchmark (manual target, not part of ctest)\n");
	std::printf("elements=%zu repeats=%u workers=%u tasks=%u\n\n",
		count, repeats, workers, taskCount);
	CaseA_JobSubmitThroughput(taskCount, workers);
	std::printf("\n");
	RunSequential<Packed48>("48B", count, repeats);
	RunSequential<Padded64>("64B-pad", count, repeats);
	std::printf("\n");
	RunRandom<Packed48>("48B", count, repeats, 1234u);
	RunRandom<Padded64>("64B-pad", count, repeats, 1234u);
	std::printf("\n");
	CaseD_HotFieldCrossLine();

	std::printf("\nchecksum=%llu (must be non-zero; prevents dead-code elimination)\n",
		static_cast<unsigned long long>(g_Sink));
	return 0;
}
