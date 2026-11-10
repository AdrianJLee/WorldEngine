// 内存基座 + 内存归因回归。
//
// 覆盖:
//   1-5  帧 arena(分配/对齐/析构/每线程/追加页)与双轨分配器(既有能力,防回归)
//   6    内存归因:标签作用域 + 每帧驱动的峰值 + 分配/释放配对
//   7    分配器登记:单一注册点 + 峰值
//   8    GPU 驻留:显式登记 + 守恒
//   9    纹理字节估算:各格式口径 + 未知格式**可见**地不计
//   10   泄漏枚举:注入已知大小的泄漏必须被定向找到
//
// 反假要点(见知识库 verification/profiling-bar):
//   * 归因断言"**恰好**增加已知字节",不是 "> 0";
//   * 泄漏断言按**地址**定向查找,不是"列表非空";
//   * 结束时打印断言计数 —— 文件被意外截断时计数会掉下来,不会静默通过。
#include "World/Core/Memory/DualTrackAllocator.h"
#include "World/Core/Memory/FrameArena.h"
#include "World/Core/Memory/LinearAllocator.h"
#include "World/Profiling/MemoryTrack.h"
#include "World/Profiling/ProfilingMacros.h"
#include "World/RHI/RhiTexture.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
	// 断言计数:文件被截断/用例被删掉时,这个数字会掉下来(防"静默通过")。
	int g_Checks = 0;

	void Check(bool condition, const char* expression, int line)
	{
		++g_Checks;
		if (!condition)
			throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
	}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)

	struct Tracked
	{
		static std::atomic<int> Live;
		static std::atomic<int> Destroyed;
		uint32_t Value = 0;

		explicit Tracked(uint32_t value) : Value(value) { ++Live; }
		~Tracked() { --Live; ++Destroyed; }
	};
	std::atomic<int> Tracked::Live { 0 };
	std::atomic<int> Tracked::Destroyed { 0 };

	// 嵌套类型不能用 using namespace 引入,显式起别名。
	using TagStat = World::Profiling::MemoryTrack::TagStat;
	using AllocatorStat = World::Profiling::MemoryTrack::AllocatorStat;
	using GpuOwnerStat = World::Profiling::MemoryTrack::GpuOwnerStat;
	using LeakEntry = World::Profiling::MemoryTrack::LeakEntry;

	// 泄漏枚举缓冲放**静态存储**:65536 × 24B = 1.5MB,放栈上会爆栈(实测 0xC00000FD)。
	LeakEntry g_LeakScan[65536];

	// 查某个标签的当前活跃字节(不存在则 0)。
	uint64_t TagLiveBytes(const char* name)
	{
		TagStat stats[World::Profiling::MemoryTrack::kMaxTags];
		const size_t count = World::Profiling::MemoryTrack::CollectTags(stats, World::Profiling::MemoryTrack::kMaxTags);
		for (size_t i = 0; i < count; ++i)
			if (stats[i].Name && std::strcmp(stats[i].Name, name) == 0)
				return stats[i].LiveBytes;
		return 0;
	}

	uint64_t TagPeakBytes(const char* name)
	{
		TagStat stats[World::Profiling::MemoryTrack::kMaxTags];
		const size_t count = World::Profiling::MemoryTrack::CollectTags(stats, World::Profiling::MemoryTrack::kMaxTags);
		for (size_t i = 0; i < count; ++i)
			if (stats[i].Name && std::strcmp(stats[i].Name, name) == 0)
				return stats[i].PeakBytes;
		return 0;
	}
}

int main()
{
	try
	{
		// 测试进程不经 Application,内存归因要显式启动(表在 Init 里一次性分配)。
		World::Profiling::MemoryTrack::Init();

		// ---- 1. 帧 arena:分配/对齐/统计 ----
		{
			World::FrameArena& arena = World::FrameArena::Get();
			void* a = arena.Allocate(64, 64);
			void* b = arena.Allocate(32, 16);
			CHECK(a != nullptr && b != nullptr);
			CHECK(reinterpret_cast<uintptr_t>(a) % 64 == 0);
			CHECK(reinterpret_cast<uintptr_t>(b) % 16 == 0);
			CHECK(arena.GetUsedMemory() >= 96);
			CHECK(arena.IsOwnerThread());
			arena.Reset();
			CHECK(arena.GetUsedMemory() == 0);
		}

		// ---- 2. Reset 时按登记析构非平凡对象 ----
		{
			World::FrameArena& arena = World::FrameArena::Get();
			Tracked::Destroyed = 0;
			auto* object = arena.New<Tracked>(7u);
			CHECK(object->Value == 7);
			CHECK(Tracked::Live.load() == 1);
			arena.Reset();
			CHECK(Tracked::Destroyed.load() == 1);
			CHECK(Tracked::Live.load() == 0);
		}

		// ---- 3. 每线程独立:工作线程的分配不改变主线程 arena 的用量 ----
		{
			World::FrameArena& mainArena = World::FrameArena::Get();
			mainArena.Reset();
			const size_t usedBefore = mainArena.GetUsedMemory();

			std::atomic<size_t> workerUsed { 0 };
			std::atomic<bool> workerOwned { false };
			std::thread worker([&]()
			{
				World::FrameArena& workerArena = World::FrameArena::Get();
				void* memory = workerArena.Allocate(256, 32);
				workerOwned = memory != nullptr && workerArena.IsOwnerThread();
				workerUsed = workerArena.GetUsedMemory();
			});
			worker.join();

			CHECK(workerOwned.load());
			CHECK(workerUsed.load() >= 256);
			CHECK(mainArena.GetUsedMemory() == usedBefore);

			std::vector<World::AllocatorStats> stats;
			CHECK(World::FrameArena::CollectStats(stats) >= 1);
			size_t used = 0, reserved = 0;
			World::FrameArena::TotalUsage(used, reserved);
			CHECK(used >= workerUsed.load());
			CHECK(reserved > 0);

			World::FrameArena::ResetAll();
			World::FrameArena::TotalUsage(used, reserved);
			CHECK(used == 0);
		}

		// ---- 4. 追加页:超过首页容量后仍可分配 ----
		{
			World::FrameArena& arena = World::FrameArena::Get();
			arena.Reset();
			std::vector<void*> blocks;
			for (int i = 0; i < 64; ++i)
				blocks.push_back(arena.Allocate(256 * 1024, 64));
			bool allValid = true;
			for (void* block : blocks)
				allValid = allValid && block != nullptr;
			CHECK(allValid);
			CHECK(arena.GetPageCount() > 1);
			arena.Reset();
		}

		// ---- 5. 双轨分配器:LOS 大对象与 SOS 小对象都可用 ----
		{
			World::DualTrackAllocator allocator("MemoryTest", 512 * 1024);
			void* small = allocator.Allocate(1024, 16);
			void* large = allocator.Allocate(2 * 1024 * 1024, 64);
			CHECK(small != nullptr && large != nullptr);
			CHECK(reinterpret_cast<uintptr_t>(large) % 64 == 0);
			allocator.Reset();
		}

		using namespace World::Profiling;

		// ---- 6. 内存归因:标签作用域 + 每帧驱动的峰值 + 分配/释放配对 ----
		{
			CHECK(MemoryTrack::Enabled());

			const uint64_t beforeLive = TagLiveBytes("MemoryTest.Tag");

			// 标签作用域内的分配必须归到该标签下(注入已知大小 ⇒ 断言**恰好**增加)。
			void* tagged = nullptr;
			{
				WLD_MEM_TAG("MemoryTest.Tag");
				tagged = ::operator new(64 * 1024);
			}
			CHECK(TagLiveBytes("MemoryTest.Tag") == beforeLive + 64 * 1024);

			// 峰值由**每帧驱动**更新,不需要外部拉取快照(旧实现的核心缺陷)。
			MemoryTrack::Tick();
			CHECK(TagPeakBytes("MemoryTest.Tag") >= beforeLive + 64 * 1024);

			// 释放后归因回落(alloc/free 配对正确)。
			::operator delete(tagged);
			CHECK(TagLiveBytes("MemoryTest.Tag") == beforeLive);

			// 标签作用域外的分配归到"未标记"(tag 0),不得误计到上个标签。
			void* untagged = ::operator new(2048);
			CHECK(TagLiveBytes("MemoryTest.Tag") == beforeLive);
			::operator delete(untagged);

			// 趋势:帧级环形采样必须真的在写(否则"趋势"是假的)。
			std::vector<uint64_t> trend(8, 0);
			const size_t trendCount = MemoryTrack::CollectTrend(trend.data(), nullptr, trend.size());
			CHECK(trendCount > 0);
		}

		// ---- 7. 分配器登记:单一注册点 + 峰值 ----
		{
			std::vector<uint8_t> buffer(64 * 1024);
			World::LinearAllocator allocator(buffer.size(), buffer.data(), "MemoryTestTracked");
			(void)allocator.Allocate(1024, 8);

			// 登记由**基类构造**自动完成:不需要(也不允许)手工注册。
			MemoryTrack::Tick();
			AllocatorStat stats[MemoryTrack::kMaxAllocators];
			const size_t count = MemoryTrack::CollectAllocators(stats, MemoryTrack::kMaxAllocators);
			size_t occurrences = 0;
			for (size_t i = 0; i < count; ++i)
			{
				if (stats[i].Name && std::strcmp(stats[i].Name, "MemoryTestTracked") == 0)
				{
					++occurrences;
					CHECK(stats[i].UsedBytes >= 1024);
					CHECK(stats[i].PeakBytes >= stats[i].UsedBytes);   // 每帧驱动,不必等 UI 拉取
					CHECK(stats[i].TotalReserved == 64 * 1024);
				}
			}
			// **恰好一次**:重复注册会让面板重复计数(PoolAllocator 曾经的缺陷)。
			CHECK(occurrences == 1);
		}

		// ---- 8b. 趋势窗口必须返回**最近**的 N 个样本(不是最旧的 N 个) ----
		//
		// 回归背景(2026-10-06 用户报"内存面板有时候不动"):环形写满后实现从**最旧**槽起取,
		// 而消费者(面板)只要 600 帧、环形有 3600 帧 ⇒ 前 10 秒正常,之后**永久冻结**在
		// 前 600 帧上。"有时候" = 取决于面板打开的时刻。
		{
			constexpr size_t kWindow = 8;
			uint64_t trend[kWindow] = {};
			size_t count = 0;

			// 每帧让显存驻留 +1KB 并 Tick,连推 40 帧(远超窗口)。
			for (uint32_t frame = 0; frame < 40; ++frame)
			{
				MemoryTrack::AddGpuResident("MemoryTest.Trend", 1024);
				MemoryTrack::Tick();
				// 注意参数顺序:(liveBytes, gpuBytes, capacity) —— 我们推的是显存,所以读第二路。
				count = MemoryTrack::CollectTrend(nullptr, trend, kWindow);
			}
			CHECK(count == kWindow);

			// 关键判据:**最后一个样本就是"现在"**。
			// 若实现返回最旧窗口,末位会停在很早的值上(这正是面板冻结的形态)。
			const uint64_t current = MemoryTrack::GpuResidentBytes();
			CHECK(trend[kWindow - 1] == current);
			CHECK(trend[0] < trend[kWindow - 1]);   // 窗口内递增 ⇒ 不是最旧那段

			for (uint32_t frame = 0; frame < 40; ++frame)
				MemoryTrack::RemoveGpuResident("MemoryTest.Trend", 1024);
		}

		// ---- 8. GPU 驻留:显式登记 + 守恒(创建 − 释放 = 驻留) ----
		{
			const uint64_t before = MemoryTrack::GpuResidentBytes();

			MemoryTrack::AddGpuResident("MemoryTest.Gpu", 4 * 1024 * 1024);
			MemoryTrack::AddGpuResident("MemoryTest.Gpu", 1 * 1024 * 1024);
			CHECK(MemoryTrack::GpuResidentBytes() == before + 5 * 1024 * 1024);

			// 按所有者分组可查(回答"是哪些资源占的")。
			{
				GpuOwnerStat owners[MemoryTrack::kMaxGpuOwners];
				const size_t count = MemoryTrack::CollectGpuOwners(owners, MemoryTrack::kMaxGpuOwners);
				bool found = false;
				for (size_t i = 0; i < count; ++i)
					if (owners[i].Name && std::strcmp(owners[i].Name, "MemoryTest.Gpu") == 0)
					{
						found = true;
						CHECK(owners[i].Bytes == 5 * 1024 * 1024);
						CHECK(owners[i].LiveCount == 2);
					}
				CHECK(found);
			}

			// 释放后**精确**回到起点(守恒)。
			MemoryTrack::RemoveGpuResident("MemoryTest.Gpu", 4 * 1024 * 1024);
			MemoryTrack::RemoveGpuResident("MemoryTest.Gpu", 1 * 1024 * 1024);
			CHECK(MemoryTrack::GpuResidentBytes() == before);

			// 释放多于登记不得出现负值(夹到 0)。
			MemoryTrack::RemoveGpuResident("MemoryTest.Gpu", 999);
			CHECK(MemoryTrack::GpuResidentBytes() == before);
		}

		// ---- 9. 纹理字节估算:格式口径 + 未知格式可见地不计 ----
		{
			using namespace World::Rhi;
			{
				TextureDesc desc;
				desc.Format = Format::R8G8B8A8_UNORM;
				desc.Extent = { 256, 256, 1 };
				desc.MipLevels = 1;
				CHECK(EstimateTextureBytes(desc) == 256ull * 256ull * 4ull);
			}
			{
				// mip 链:256² + 128² + ... + 1²,每像素 4B。
				TextureDesc desc;
				desc.Format = Format::R8G8B8A8_UNORM;
				desc.Extent = { 256, 256, 1 };
				desc.MipLevels = 9;
				const uint64_t expected =
					(256ull * 256 + 128 * 128 + 64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1 * 1) * 4;
				CHECK(EstimateTextureBytes(desc) == expected);
			}
			{
				// BC1:8 字节 / 4×4 块。
				TextureDesc desc;
				desc.Format = Format::BC1_UNORM;
				desc.Extent = { 64, 64, 1 };
				desc.MipLevels = 1;
				CHECK(EstimateTextureBytes(desc) == 16ull * 16ull * 8ull);
			}
			{
				// 未知格式必须返回 0 并把计数 +1(而不是静默当成 0 字节)。
				const uint64_t beforeUnaccounted = UnaccountedTextureCount();
				TextureDesc desc;
				desc.Format = Format::Undefined;
				desc.Extent = { 64, 64, 1 };
				desc.MipLevels = 1;
				CHECK(EstimateTextureBytes(desc) == 0);
				CHECK(UnaccountedTextureCount() == beforeUnaccounted + 1);
			}
		}

		// ---- 10. 泄漏枚举:注入已知大小的泄漏必须被**定向**找到 ----
		{
			// 进程内还有大量其它活跃分配,所以不能只断言"列表非空" —— 那会假通过。
			void* leaked = ::operator new(1234);
			{
				const size_t count = MemoryTrack::CollectLeaks(g_LeakScan, 65536);
				bool found = false;
				for (size_t i = 0; i < count; ++i)
					if (g_LeakScan[i].Address == leaked)
					{
						found = true;
						CHECK(g_LeakScan[i].Size == 1234);
					}
				CHECK(found);
			}
			::operator delete(leaked);
		}

		// 断言计数:文件被截断或用例被删掉时这里会掉下来(防静默通过)。
		CHECK(g_Checks >= 44);

		World::Profiling::MemoryTrack::Shutdown();

		std::printf("World.Memory: all checks passed (%d checks)\n", g_Checks);
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Memory: FAILED after %d checks: %s\n", g_Checks, error.what());
		return 1;
	}
}
