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
#include "World/Math/Math.h"
#include <glm/gtc/matrix_transform.hpp>
#include "World/Renderer/FrustumCull.h"
#include "World/Math/Simd/FrustumCullSimd.h"
#include "World/Math/Math.h"

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

	// 编译期架构标签:同源 O3 对照靠它区分两个目标(SSE2 基线 vs /arch:AVX2)。
#if defined(__AVX2__)
	constexpr const char* kArchLabel = "AVX2";
#elif defined(__AVX__)
	constexpr const char* kArchLabel = "AVX";
#else
	constexpr const char* kArchLabel = "SSE2";
#endif

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

	// ================= O2 / O3 / O4 的实测用例(标准 §6) =================

	// 用例 E(O2):物理插值状态 **改前 68B(整矩阵 + bool)** vs **改后 44B(权威 TRS)**。
	// O2 已于 2026-10-05 落地(44B);这里保留 68B 变体作为"改前"对照,让这条裁决可复跑复现,
	// 68B = mat4(64) + bool(1) + 3B 填充;44B = vec3(12) + quat(16) + vec3(12) + bool(1) + 3B 填充。
	// 68B 跨两条 cache line,44B 装进一条 —— 这才是 O2 的真正价值,不是"省 24B"。
	struct Interp68
	{
		glm::mat4 PreviousLocalMatrix { 1.0f };
		bool Valid = false;
	};
	static_assert(sizeof(Interp68) == 68, "Interp68 must mirror PhysicsInterpolationState (68B)");

	struct Interp44
	{
		glm::vec3 PreviousLocation { 0.0f };
		glm::quat PreviousRotation { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 PreviousScale { 1.0f };
		bool Valid = false;
	};
	static_assert(sizeof(Interp44) == 44, "Interp44 must be the decomposed 44B candidate");

	// 每元素读**自己的**首字段(offset 0):这才是"遍历一遍插值状态"的诚实口径 ——
	// 读元素内的固定偏移会踩到相邻元素(44B 步长下 offset 48 属于下一个元素),那是探针错误。
	// 差异因此纯粹来自 stride ⇒ cache line 数:68B = 1.062 线/元素,44B = 0.688 线/元素。
	template <typename T>
	void RunInterpRead(const char* label, std::size_t count, uint32_t repeats)
	{
		std::vector<T> data(count);
		for (std::size_t i = 0; i < count; ++i)
			reinterpret_cast<float*>(&data[i])[0] = static_cast<float>(i);

		const auto begin = Clock::now();
		float acc = 0.0f;
		for (uint32_t r = 0; r < repeats; ++r)
			for (std::size_t i = 0; i < count; ++i)
				acc += reinterpret_cast<const float*>(&data[i])[0];
		const auto end = Clock::now();

		const double ms = Millis(begin, end);
		const double elements = static_cast<double>(count) * repeats;
		std::printf("E. O2 %-12s : %8.2f ms | %8.3f ns/elem | %.3f lines/elem (stride %zuB)\n",
			label, ms, ms * 1.0e6 / elements,
			static_cast<double>(sizeof(T)) / 64.0, sizeof(T));
		g_Sink += static_cast<uint64_t>(acc);
	}

	// 用例 F(O3):热变换数学(TRS → 矩阵),量 /arch:AVX2 的实际差距。
	// 同一份源码编两次(见 CMake:WorldLayoutBenchmarksAvx2),只有 /arch 不同。
	void CaseF_TransformMath(std::size_t count, uint32_t repeats)
	{
		std::vector<glm::vec3> loc(count), scl(count);
		std::vector<glm::quat> rot(count);
		std::vector<glm::mat4> out(count);
		for (std::size_t i = 0; i < count; ++i)
		{
			loc[i] = glm::vec3(static_cast<float>(i), 0.0f, 0.0f);
			scl[i] = glm::vec3(1.0f);
			rot[i] = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		}

		const auto begin = Clock::now();
		for (uint32_t r = 0; r < repeats; ++r)
			for (std::size_t i = 0; i < count; ++i)
				out[i] = World::TransformSystem::Compose(loc[i], rot[i], scl[i]);
		const auto end = Clock::now();

		const double ms = Millis(begin, end);
		const double elements = static_cast<double>(count) * repeats;
		std::printf("F. O3 compose[%s] : %8.2f ms | %8.3f ns/elem\n",
			kArchLabel, ms, ms * 1.0e6 / elements);
		g_Sink += static_cast<uint64_t>(out[0][0][0]);
	}

	// 用例 G(O4):对齐的 mat4 加载是否更快。
	// 现状 alignof(glm::mat4) 见输出;GLM_FORCE_ALIGNED_GENTYPES 会把它抬到 16。
	// 对照:packed(alignof 4)vs alignas(16) 的 64B 矩阵数组做同一段乘法。
	struct PackedMat4 { float M[16]; };
	static_assert(sizeof(PackedMat4) == 64, "PackedMat4 = 16 floats");
	struct alignas(16) AlignedMat4 { float M[16]; };
	static_assert(sizeof(AlignedMat4) == 64 && alignof(AlignedMat4) == 16, "AlignedMat4 must be 16B aligned");

	template <typename M>
	void RunMatMul(const char* label, std::size_t count, uint32_t repeats)
	{
		std::vector<M> data(count);
		for (std::size_t i = 0; i < count; ++i)
			for (int k = 0; k < 16; ++k)
				data[i].M[k] = static_cast<float>(k) + static_cast<float>(i);

		std::vector<float> acc(count, 0.0f);
		const auto begin = Clock::now();
		for (uint32_t r = 0; r < repeats; ++r)
			for (std::size_t i = 0; i < count; ++i)
			{
				float sum = 0.0f;
				for (int k = 0; k < 16; ++k)
					sum += data[i].M[k] * data[i].M[(k + 1) & 15];
				acc[i] += sum;
			}
		const auto end = Clock::now();

		const double ms = Millis(begin, end);
		const double elements = static_cast<double>(count) * repeats;
		std::printf("G. O4 %-12s : %8.2f ms | %8.3f ns/elem | alignof=%zu\n",
			label, ms, ms * 1.0e6 / elements, alignof(M));
		g_Sink += static_cast<uint64_t>(acc[0]);
	}

	// 用例 H(O1):2D quad 顶点变换 —— 手写 SSE vs glm。
	//
	// 这是标准 §6.5 / §6.5.7 里对现存实现的裁定实验。手写版逐分量 `_mm_set1_ps` 广播去
	// 处理**单个** vec4(既不是 SoA 也不是批量),而 glm 在 x64 上本身就是 SSE2 ⇒ 预期手写版
	// 没有优势,还多了 `_mm_storeu_ps` 到临时数组再逐元素拷贝的 store-forwarding 惩罚。
	// 结论由数字定:无收益就删掉手写版、统一走 glm(单一实现)。
	void CaseH_QuadTransformHandrolledVsGlm(std::size_t quadCount, uint32_t repeats)
	{
		std::vector<glm::mat4> transforms(quadCount);
		for (std::size_t i = 0; i < quadCount; ++i)
			transforms[i] = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(i) * 0.01f, 0.0f, 0.0f))
				* glm::rotate(glm::mat4(1.0f), static_cast<float>(i) * 0.001f, glm::vec3(0.0f, 0.0f, 1.0f));

		// 单位性:quad 的 4 个角(与 Renderer2D::s_Data.VertexPositions 同形)。
		const glm::vec4 corners[4] = {
			{ -0.5f, -0.5f, 0.0f, 1.0f }, { 0.5f, -0.5f, 0.0f, 1.0f },
			{ 0.5f, 0.5f, 0.0f, 1.0f }, { -0.5f, 0.5f, 0.0f, 1.0f } };

		// 输出必须真正被消费,否则编译器可能把整段循环删掉。
		double checksum = 0.0;

		const auto beginHand = Clock::now();
		for (uint32_t r = 0; r < repeats; ++r)
			for (std::size_t i = 0; i < quadCount; ++i)
			{
				glm::vec3 out[4];
				World::Math::MultiplyMat4ByVec4_SIMD_x4(transforms[i], corners, out);
				checksum += static_cast<double>(out[0].x) + static_cast<double>(out[2].y);
			}
		const auto endHand = Clock::now();

		const auto beginGlm = Clock::now();
		for (uint32_t r = 0; r < repeats; ++r)
			for (std::size_t i = 0; i < quadCount; ++i)
			{
				glm::vec3 out[4];
				for (int c = 0; c < 4; ++c)
					out[c] = glm::vec3(transforms[i] * corners[c]);
				checksum += static_cast<double>(out[0].x) + static_cast<double>(out[2].y);
			}
		const auto endGlm = Clock::now();

		// 变体 C(**规则 P0 要求的"先用库表达"**):同样的"矩阵列提升到顶点循环外"结构,
		// 但用 glm 的 vec4 运算写 —— 与手写版同操作数、同顺序,只是不含 intrinsics。
		// 若它与手写版打平,则按 §6.5.1 P0 应当保留 glm 版(可移植、无需门禁例外);
		// 只有它明显更慢时,才轮到"手写 intrinsics"并需要登记理由。
		const auto beginHoistedGlm = Clock::now();
		for (uint32_t r = 0; r < repeats; ++r)
			for (std::size_t i = 0; i < quadCount; ++i)
			{
				const glm::mat4& m = transforms[i];
				const glm::vec4 c0(m[0]), c1(m[1]), c2(m[2]), c3(m[3]);
				glm::vec3 out[4];
				for (int c = 0; c < 4; ++c)
				{
					const glm::vec4 v = corners[c];
					out[c] = glm::vec3(c0 * v.x + c1 * v.y + c2 * v.z + c3 * v.w);
				}
				checksum += static_cast<double>(out[0].x) + static_cast<double>(out[2].y);
			}
		const auto endHoistedGlm = Clock::now();

		const double hoistedGlmMs = Millis(beginHoistedGlm, endHoistedGlm);
		const double handMs = Millis(beginHand, endHand);
		const double glmMs = Millis(beginGlm, endGlm);
		const double elements = static_cast<double>(quadCount) * repeats;
		std::printf("H. O1 quad xform  : hand %8.2f | glm %8.2f | glm-hoisted %8.2f ms | %7.3f / %7.3f / %7.3f ns/quad | hand/hoisted=%.3f\n",
			handMs, glmMs, hoistedGlmMs, handMs * 1.0e6 / elements, glmMs * 1.0e6 / elements,
			hoistedGlmMs * 1.0e6 / elements, handMs / hoistedGlmMs);
		g_Sink += static_cast<uint64_t>(checksum);
	}

	// 用例 I(B 轴):视锥剔除 —— 逐对象标量 vs 4 对象/lane 批处理 SIMD。
	// 这是标准 §6.5.0 **B 轴**的裁定实验:标量版每平面可能早退(6 次/对象分支压力),
	// 批处理版按平面循环、无分支。
	void CaseI_FrustumCullScalarVsSimd(std::size_t drawCount, uint32_t repeats)
	{
		using namespace World;
		const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
		const FrustumPlanes frustum = ExtractFrustumPlanes(proj * view);

		std::mt19937 rng(4242u);
		std::uniform_real_distribution<float> pos(-8.0f, 8.0f);
		std::uniform_real_distribution<float> ext(0.05f, 1.5f);
		std::vector<glm::vec3> mins(drawCount), maxs(drawCount);
		for (std::size_t i = 0; i < drawCount; ++i)
		{
			mins[i] = glm::vec3(pos(rng), pos(rng), pos(rng));
			maxs[i] = mins[i] + glm::vec3(ext(rng), ext(rng), ext(rng));
		}
		std::vector<std::uint8_t> mask(drawCount, 0);

		std::size_t scalarVisible = 0;
		const auto beginScalar = Clock::now();
		for (uint32_t r = 0; r < repeats; ++r)
		{
			scalarVisible = 0;
			for (std::size_t i = 0; i < drawCount; ++i)
				scalarVisible += AabbInFrustum(frustum, mins[i], maxs[i]) ? 1u : 0u;
		}
		const auto endScalar = Clock::now();

		std::size_t simdVisible = 0;
		const auto beginSimd = Clock::now();
		for (uint32_t r = 0; r < repeats; ++r)
		{
			Math::Simd::AabbInFrustumBatch(frustum.Planes, mins.data(), maxs.data(), drawCount, mask.data());
			simdVisible = 0;
			for (std::size_t i = 0; i < drawCount; ++i)
				simdVisible += mask[i];
		}
		const auto endSimd = Clock::now();

		// 变体 C:**渲染器实际走的那条路** —— 分块 + AoS→SoA 取样 + 批量判定。
		// 只测 batch 函数会漏掉取样开销;把 tiled 也量出来,裁定才贴着真实路径。
		std::size_t tiledVisible = 0;
		const auto beginTiled = Clock::now();
		for (uint32_t r = 0; r < repeats; ++r)
		{
			Math::Simd::AabbInFrustumTiled(frustum.Planes, drawCount,
				[&](std::size_t idx, glm::vec3& outMin, glm::vec3& outMax)
				{
					outMin = mins[idx];
					outMax = maxs[idx];
				},
				mask.data());
			tiledVisible = 0;
			for (std::size_t k = 0; k < drawCount; ++k)
				tiledVisible += mask[k];
		}
		const auto endTiled = Clock::now();
		const double elements = static_cast<double>(drawCount) * repeats;
		const double scalarMs = Millis(beginScalar, endScalar);
		const double simdMs = Millis(beginSimd, endSimd);
		const double tiledMs = Millis(beginTiled, endTiled);
		std::printf("I. B-axis cull    : scalar %7.2f | simd %7.2f | tiled %7.2f ms | %7.3f / %7.3f / %7.3f ns/draw | scalar/tiled=%.2fx | visible %zu/%zu (%zu tiled)\n",
			scalarMs, simdMs, tiledMs, scalarMs * 1.0e6 / elements, simdMs * 1.0e6 / elements,
			tiledMs * 1.0e6 / elements, scalarMs / tiledMs, simdVisible, drawCount, tiledVisible);
		g_Sink += static_cast<uint64_t>(scalarVisible + simdVisible + tiledVisible);
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
	std::printf("\n");
	RunInterpRead<Interp68>("68B (mat4)", count, repeats);
	RunInterpRead<Interp44>("44B (TRS)", count, repeats);
	std::printf("\n");
	CaseF_TransformMath(count, repeats);
	std::printf("\n");
	RunMatMul<PackedMat4>("packed(4)", count, repeats);
	RunMatMul<AlignedMat4>("align16", count, repeats);
	std::printf("   alignof(glm::mat4)=%zu (当前 = packed;GLM_FORCE_ALIGNED_GENTYPES 会把它抬到 16)\n", alignof(glm::mat4));
	std::printf("\n");
	std::printf("\n");
	CaseH_QuadTransformHandrolledVsGlm(static_cast<std::size_t>(1) << 12, 8u);
	std::printf("\n");
	CaseI_FrustumCullScalarVsSimd(static_cast<std::size_t>(1) << 14, 16u);
	CaseD_HotFieldCrossLine();

	std::printf("\nchecksum=%llu (must be non-zero; prevents dead-code elimination)\n",
		static_cast<unsigned long long>(g_Sink));
	return 0;
}
