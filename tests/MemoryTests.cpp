#include "World/Core/Memory/DualTrackAllocator.h"
#include "World/Core/Memory/FrameArena.h"
#include "World/Core/Memory/PoolAllocator.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
	void Check(bool condition, const char* expression, int line)
	{
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
}

int main()
{
	try
	{
		// 1. 帧 arena:分配/对齐/统计
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

		// 2. Reset 时按登记析构非平凡对象
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

		// 3. 每线程独立:工作线程的分配不改变主线程 arena 的用量
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

			// ResetAll:全部线程(含已退出线程)的 arena 一并回卷。
			World::FrameArena::ResetAll();
			World::FrameArena::TotalUsage(used, reserved);
			CHECK(used == 0);
		}

		// 4. 追加页:超过首页容量后仍可分配,且能力上限内不失败
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

		// 5. 双轨分配器(引擎/主线程用):LOS 大对象与 SOS 小对象都可用
		{
			World::DualTrackAllocator allocator("MemoryTest", 512 * 1024);
			void* small = allocator.Allocate(1024, 16);
			void* large = allocator.Allocate(2 * 1024 * 1024, 64);
			CHECK(small != nullptr && large != nullptr);
			CHECK(reinterpret_cast<uintptr_t>(large) % 64 == 0);
			allocator.Reset();
		}

		// 6. 池分配器:同 Tag 复用、计数正确
		{
			World::PoolAllocator pool("MemoryTestPool", World::PoolTag::General, sizeof(Tracked), alignof(Tracked), 8);
			void* first = pool.Allocate(sizeof(Tracked), alignof(Tracked));
			void* second = pool.Allocate(sizeof(Tracked), alignof(Tracked));
			CHECK(first != nullptr && second != nullptr && first != second);
			pool.Deallocate(first);
			void* reused = pool.Allocate(sizeof(Tracked), alignof(Tracked));
			CHECK(reused == first);
			pool.Deallocate(second);
			pool.Deallocate(reused);
		}

		World::FrameArena::Shutdown();
		std::printf("World.Memory: all checks passed\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "World.Memory: FAILED: %s\n", error.what());
		return 1;
	}
}
