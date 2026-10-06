#include "wldpch.h"
#include "World/Profiling/MemoryTrack.h"

#include "World/Core/Memory/Allocator.h"
#include "World/Profiling/SymbolResolver.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>

#if defined(WLD_PLATFORM_WINDOWS)
#include <intrin.h> // _ReturnAddress:调用点采样(非 SIMD,不受门禁 E 管辖)
#endif
#include <thread>
#include <vector>

#if defined(WLD_PLATFORM_WINDOWS)
#include <intrin.h> // _mm_pause:自旋锁退避提示
#endif

namespace World::Profiling
{
	namespace
	{
		// 指针表的槽位:开放寻址 + 线性探测。尺寸 24 字节。
		struct Slot
		{
			uintptr_t Key = 0;      // 0 = 空
			uint64_t Size = 0;
			void* Origin = nullptr; // 采样到的调用点;nullptr = 未采样
			uint32_t Tag = 0;
			uint32_t SiteIndex = 0xFFFFFFFFu; // 站点聚合表下标(0xFFFFFFFF = 无)
		};

		// 分片自旋锁:分配路径并发度高,单锁会成为瓶颈。64 片足够分散。
		constexpr uint32_t kShardCount = 64;
		// 临界区只覆盖“写/清一个哈希槽”(纳秒级),自旋 + 退让足够。
		// 刻意不用平台 intrinsic(如 _mm_pause):那会触发 SIMD 门禁 E
		// (手写 intrinsics 只允许出现在 Math/Simd,性能标准 §6.5.2 S1),
		// 为一个槽位锁申请 SIMD 豁免不成比例;退让用标准库即可。
		// 临界区只覆盖“写/清一个哈希槽”(纳秒级),自旋 + 退让足够。
		// 刻意不用平台 intrinsic(如 _mm_pause):那会触发 SIMD 门禁 E
		// (手写 intrinsics 只允许出现在 Math/Simd,性能标准 §6.5.2 S1),
		// 为一个槽位锁申请 SIMD 豁免不成比例;退让用标准库即可。
		class SpinLock
		{
		public:
			void Lock() noexcept
			{
				int spins = 0;
				while (m_Flag.test_and_set(std::memory_order_acquire))
				{
					if (++spins > 64)
					{
						std::this_thread::yield();
						spins = 0;
					}
				}
			}
			void Unlock() noexcept { m_Flag.clear(std::memory_order_release); }
		private:
			std::atomic_flag m_Flag = ATOMIC_FLAG_INIT;
		};

		// 标签槽:名字用原子指针(写一次),统计用原子计数。
		struct TagSlot
		{
			std::atomic<const char*> Name { nullptr };
			std::atomic<uint64_t> LiveBytes { 0 };
			std::atomic<uint64_t> PeakBytes { 0 };
			std::atomic<uint64_t> LiveCount { 0 };
			std::atomic<uint64_t> TotalAllocs { 0 };
			std::atomic<uint64_t> TotalFrees { 0 };
		};

		struct AllocatorSlot
		{
			std::atomic<Allocator*> Ptr { nullptr };
			std::atomic<uint64_t> PeakBytes { 0 };
		};

		struct GpuSlot
		{
			std::atomic<const char*> Name { nullptr };
			std::atomic<uint64_t> Bytes { 0 };
			std::atomic<uint64_t> LiveCount { 0 };
		};
	}

	class MemoryTrackState
	{
	public:
		bool Enabled = false;
		uint32_t Capacity = 0;              // 2 的幂
		uint32_t Mask = 0;
		Slot* Slots = nullptr;
		SpinLock Shards[kShardCount];

		TagSlot Tags[MemoryTrack::kMaxTags];
		std::atomic<uint32_t> TagCount { 0 };

		AllocatorSlot Allocators[MemoryTrack::kMaxAllocators];
		std::atomic<uint32_t> AllocatorCount { 0 };
		// 分配器列表的写者(构造/析构)与读者(Tick)竞争:用一把小锁串行化槽位分配。
		std::mutex AllocatorMutex;

		GpuSlot Gpu[MemoryTrack::kMaxGpuOwners];
		std::atomic<uint32_t> GpuCount { 0 };

		std::atomic<uint64_t> HeapLive { 0 };
		std::atomic<uint64_t> HeapPeak { 0 };
		std::atomic<uint64_t> HeapLiveCount { 0 };
		std::atomic<uint64_t> TotalAllocs { 0 };
		std::atomic<uint64_t> UnknownFrees { 0 };
		std::atomic<uint64_t> GpuTotal { 0 };
		std::atomic<uint64_t> InsertFailures { 0 }; // 表满:归因漏记(可见,不静默)

		// ---- 站点(Site)表:调用点 → 次数/字节。低基数(实际几百个),线性探测足够。 ----
		static constexpr uint32_t kMaxSites = 4096;
		struct SiteSlot
		{
			std::atomic<void*> Origin { nullptr };
			std::atomic<uint64_t> LiveBytes { 0 };
			std::atomic<uint64_t> LiveCount { 0 };
			std::atomic<uint64_t> TotalAllocs { 0 };
		};
		SiteSlot Sites[kMaxSites];
		std::atomic<uint32_t> SiteCount { 0 };
		std::atomic<uint32_t> SiteSampling { 0 };      // 0 = 关闭
		std::atomic<uint64_t> SiteSampleCounter { 0 };

		// 帧趋势:环形。
		uint64_t TrendLive[MemoryTrack::kMaxTrendFrames] = {};
		uint64_t TrendGpu[MemoryTrack::kMaxTrendFrames] = {};
		uint32_t TrendWrite = 0;
		uint32_t TrendCount = 0;

		static uint32_t NextPow2(uint32_t value)
		{
			uint32_t result = 1;
			while (result < value)
				result <<= 1;
			return result;
		}
	};

	namespace
	{
		MemoryTrackState& State()
		{
			static MemoryTrackState state;
			return state;
		}

		// 站点下标哨兵("这次分配没被采样到")。
		constexpr uint32_t kNoSite = 0xFFFFFFFFu;

		// 采样式取返回地址。**不做整栈回溯**:成本可控且足够定位到分配点;
		// 需要完整调用链时用外部 profiler(那是明确的非目标,见方案 §1.2)。
		void* AddressOfCaller() noexcept
		{
#if defined(WLD_PLATFORM_WINDOWS)
			return _ReturnAddress();
#else
			return nullptr;
#endif
		}

		// 递归保护:簿记本身若触发分配会无限递归(new 里再 new)。
		// 注意用 `int` 而非 bool:hook 可能嵌套(例如 malloc 内部)。
		thread_local int t_TrackDepth = 0;
		thread_local uint32_t t_TagStack[64] = {};
		thread_local uint32_t t_TagDepth = 0;
		// 上次用过的标签:避免热路径每次线性扫描 256 个槽。
		thread_local const char* t_LastTagName = nullptr;
		thread_local uint32_t t_LastTagId = 0;

		class ReentryGuard
		{
		public:
			ReentryGuard() noexcept { ++t_TrackDepth; }
			~ReentryGuard() { --t_TrackDepth; }
		};

		uint32_t ShardFor(uintptr_t key) noexcept
		{
			// 指针通常按 16 对齐 ⇒ 低位全是 0,必须混一下再取模。
			uint64_t mixed = static_cast<uint64_t>(key) >> 4;
			mixed ^= mixed >> 33;
			mixed *= 0xff51afd7ed558ccdull;
			mixed ^= mixed >> 33;
			return static_cast<uint32_t>(mixed) & (kShardCount - 1);
		}

		size_t ProbeStart(uintptr_t key, uint32_t capacity) noexcept
		{
			uint64_t mixed = static_cast<uint64_t>(key) >> 4;
			mixed ^= mixed >> 33;
			mixed *= 0xc4ceb9fe1a85ec53ull;
			mixed ^= mixed >> 33;
			return static_cast<size_t>(mixed & (capacity - 1));
		}
	}

	MemoryTrack& MemoryTrack::Get()
	{
		static MemoryTrack instance;
		return instance;
	}

	bool MemoryTrack::Enabled() noexcept
	{
		return State().Enabled;
	}

	// ---------------------------------------------------------------------------
	// 生命周期
	// ---------------------------------------------------------------------------

	void MemoryTrack::Init()
	{
		MemoryTrackState& state = State();
		if (state.Enabled)
			return;

		// 表在 Init 时一次性分配:之后 RecordAllocate 路径上不允许再分配(见 R2)。
		uint32_t slots = 1u << 19; // 524288 槽 × 24B ≈ 12 MiB
		if (const char* env = std::getenv("WLD_MEMORY_TRACK_SLOTS"))
		{
			const long parsed = std::strtol(env, nullptr, 10);
			if (parsed > 0)
				slots = MemoryTrackState::NextPow2(static_cast<uint32_t>(parsed));
		}
		state.Capacity = slots;
		state.Mask = slots - 1;
		state.Slots = new (std::nothrow) Slot[slots];
		if (!state.Slots)
		{
			WLD_CORE_ERROR("[memory] MemoryTrack: allocation table ({0} slots) failed; tracking disabled", slots);
			state.Capacity = 0;
			return;
		}
		std::memset(state.Slots, 0, sizeof(Slot) * slots);
		// 调用点采样率(WLD_MEMORY_SITES=N ⇒ 1/N 采样;默认 64)。
		// 采样代价 = 一次 _ReturnAddress + 一次原子加,热路径上不做任何分配。
		{
			uint32_t rate = 64;
			if (const char* env = std::getenv("WLD_MEMORY_SITES"))
			{
				const long parsed = std::strtol(env, nullptr, 10);
				rate = parsed <= 0 ? 0u : static_cast<uint32_t>(parsed);
			}
			state.SiteSampling.store(rate, std::memory_order_relaxed);
		}
		state.Enabled = true;
		WLD_CORE_INFO("[memory] MemoryTrack enabled ({0} slots, ~{1} MiB)", slots,
			(sizeof(Slot) * slots) / (1024 * 1024));
	}

	void MemoryTrack::Shutdown()
	{
		MemoryTrackState& state = State();
		if (!state.Enabled)
			return;
		// 报告未释放的分配(在表被释放**之前**:这正是旧实现搞错顺序的地方)。
		{
			char buffer[256];
			const uint64_t live = state.HeapLive.load(std::memory_order_relaxed);
			const uint64_t count = state.HeapLiveCount.load(std::memory_order_relaxed);
			if (count > 0)
			{
				std::snprintf(buffer, sizeof(buffer),
					"[memory] %llu live heap allocation(s), %llu bytes at shutdown",
					static_cast<unsigned long long>(count), static_cast<unsigned long long>(live));
				WLD_CORE_WARN("{0}", buffer);
			}
			else
			{
				WLD_CORE_INFO("[memory] heap balanced at shutdown (all tracked allocations released)");
			}
			const uint64_t unknown = state.UnknownFrees.load(std::memory_order_relaxed);
			const uint64_t failures = state.InsertFailures.load(std::memory_order_relaxed);
			if (unknown > 0 || failures > 0)
			{
				WLD_CORE_WARN("[memory] attribution imperfect: {0} untracked free(s), {1} insert failure(s)",
					unknown, failures);
			}
		}

		state.Enabled = false;
		delete[] state.Slots;
		state.Slots = nullptr;
		state.Capacity = 0;
	}

	// ---------------------------------------------------------------------------
	// 标签
	// ---------------------------------------------------------------------------

	uint32_t MemoryTrack::AcquireTag(const char* name) noexcept
	{
		if (!name || name[0] == '\0')
			return 0;
		if (name == t_LastTagName)
			return t_LastTagId;

		MemoryTrackState& state = State();
		const uint32_t count = state.TagCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < count; ++i)
		{
			const char* existingName = state.Tags[i].Name.load(std::memory_order_relaxed);
			// 先比指针(热路径常见:同一字面量),**必须**回退到内容比较 ——
			// 同名标签来自不同翻译单元时字面量指针不同,只比指针会把同一逻辑标签拆成两条。
			if (existingName == name || (existingName && std::strcmp(existingName, name) == 0))
			{
				t_LastTagName = name;
				t_LastTagId = i + 1;
				return i + 1;
			}
		}

		// 新标签:只在首次出现时走这条路径(不在热路径上)。
		const uint32_t slotIndex = count;
		if (slotIndex >= kMaxTags)
			return 0;
		state.Tags[slotIndex].Name.store(name, std::memory_order_relaxed);
		state.TagCount.store(slotIndex + 1, std::memory_order_release);
		t_LastTagName = name;
		t_LastTagId = slotIndex + 1;
		return slotIndex + 1;
	}

	void MemoryTrack::PushTag(const char* tag) noexcept
	{
		if (!State().Enabled)
			return;
		const uint32_t id = Get().AcquireTag(tag);
		if (t_TagDepth < 64)
			t_TagStack[t_TagDepth++] = id;
	}

	void MemoryTrack::PopTag() noexcept
	{
		if (t_TagDepth > 0)
			--t_TagDepth;
	}

	const char* MemoryTrack::CurrentTagName() noexcept
	{
		if (t_TagDepth == 0)
			return nullptr;
		MemoryTrackState& state = State();
		const uint32_t id = t_TagStack[t_TagDepth - 1];
		if (id == 0)
			return nullptr;
		return state.Tags[id - 1].Name.load(std::memory_order_relaxed);
	}

	// ---------------------------------------------------------------------------
	// 全局堆记录
	// ---------------------------------------------------------------------------

	void MemoryTrack::RecordAllocate(void* ptr, size_t size, void* origin) noexcept
	{
		MemoryTrackState& state = State();
		// Suppressed:符号化等诊断自身的分配不计入归因(否则观察者污染被观察对象)。
		if (!state.Enabled || !ptr || SymbolResolver::Suppressed())
			return;

		const uint32_t tagId = (t_TagDepth > 0) ? t_TagStack[t_TagDepth - 1] : 0;

		// 调用点采样:1/N。origin 由 hook 传入(见头文件说明)。
		// 未被采样的分配不建表项 —— 采样本身就省掉了建表与后续聚合的代价。
		const uint32_t rate = state.SiteSampling.load(std::memory_order_relaxed);
		if (rate == 0 || (state.SiteSampleCounter.fetch_add(1, std::memory_order_relaxed) % rate) != 0)
			origin = nullptr;

		const uint32_t siteIndex = origin ? Get().AcquireSite(origin) : kNoSite;
		if (siteIndex != kNoSite)
		{
			MemoryTrackState::SiteSlot& site = state.Sites[siteIndex];
			site.LiveBytes.fetch_add(size, std::memory_order_relaxed);
			site.LiveCount.fetch_add(1, std::memory_order_relaxed);
			site.TotalAllocs.fetch_add(1, std::memory_order_relaxed);
		}

		Get().InsertRecord(ptr, size, tagId, origin, siteIndex);

		state.HeapLive.fetch_add(size, std::memory_order_relaxed);
		state.HeapLiveCount.fetch_add(1, std::memory_order_relaxed);
		state.TotalAllocs.fetch_add(1, std::memory_order_relaxed);
		if (tagId != 0)
		{
			TagSlot& slot = state.Tags[tagId - 1];
			slot.LiveBytes.fetch_add(size, std::memory_order_relaxed);
			slot.LiveCount.fetch_add(1, std::memory_order_relaxed);
			slot.TotalAllocs.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void MemoryTrack::RecordDeallocate(void* ptr) noexcept
	{
		MemoryTrackState& state = State();
		if (!state.Enabled || !ptr || SymbolResolver::Suppressed())
			return;

		size_t size = 0;
		uint32_t tagId = 0;
		uint32_t siteIndex = kNoSite;
		if (!Get().EraseRecord(ptr, &size, &tagId, &siteIndex))
		{
			// 表里没有:该分配不是在装了 hook 的模块里做的,或表曾经满过。
			// **不能**猜大小;只计数,让"归因偏大"这件事本身可见。
			state.UnknownFrees.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		state.HeapLive.fetch_sub(size, std::memory_order_relaxed);
		state.HeapLiveCount.fetch_sub(1, std::memory_order_relaxed);
		if (siteIndex != kNoSite && siteIndex < MemoryTrackState::kMaxSites)
		{
			MemoryTrackState::SiteSlot& site = state.Sites[siteIndex];
			const uint64_t current = site.LiveBytes.load(std::memory_order_relaxed);
			site.LiveBytes.store(current >= size ? current - size : 0, std::memory_order_relaxed);
			const uint64_t count = site.LiveCount.load(std::memory_order_relaxed);
			if (count > 0)
				site.LiveCount.store(count - 1, std::memory_order_relaxed);
		}
		if (tagId != 0)
		{
			TagSlot& slot = state.Tags[tagId - 1];
			slot.LiveBytes.fetch_sub(size, std::memory_order_relaxed);
			slot.LiveCount.fetch_sub(1, std::memory_order_relaxed);
			slot.TotalFrees.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void MemoryTrack::InsertRecord(void* ptr, size_t size, uint32_t tagId, void* origin, uint32_t siteIndex) noexcept
	{
		MemoryTrackState& state = State();
		const uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
		const uint32_t shard = ShardFor(key);
		SpinLock& lock = state.Shards[shard];
		lock.Lock();

		const uint32_t capacity = state.Capacity;
		size_t index = ProbeStart(key, capacity);
		for (uint32_t probe = 0; probe < capacity; ++probe)
		{
			Slot& slot = state.Slots[index];
			if (slot.Key == 0)
			{
				slot.Key = key;
				slot.Size = size;
				slot.Tag = tagId;
				slot.Origin = origin;
				slot.SiteIndex = siteIndex;
				lock.Unlock();
				return;
			}
			if (slot.Key == key)
			{
				// 同一地址重复出现 ⇒ 上一轮记录没被释放(可能的泄漏/双重分配)。
				// 覆盖并计数,不放大内存占用。
				slot.Size = size;
				slot.Tag = tagId;
				lock.Unlock();
				return;
			}
			index = (index + 1) & state.Mask;
		}

		state.InsertFailures.fetch_add(1, std::memory_order_relaxed);
		lock.Unlock();
	}

	bool MemoryTrack::EraseRecord(void* ptr, size_t* sizeOut, uint32_t* tagOut, uint32_t* siteOut) noexcept
	{
		MemoryTrackState& state = State();
		const uintptr_t key = reinterpret_cast<uintptr_t>(ptr);
		const uint32_t shard = ShardFor(key);
		SpinLock& lock = state.Shards[shard];
		lock.Lock();

		size_t index = ProbeStart(key, state.Capacity);
		for (uint32_t probe = 0; probe < state.Capacity; ++probe)
		{
			Slot& slot = state.Slots[index];
			if (slot.Key == 0)
			{
				lock.Unlock();
				return false;
			}
			if (slot.Key == key)
			{
				*sizeOut = slot.Size;
				*tagOut = slot.Tag;

				// 线性探测下的删除:把后续"本该更靠前"的元素回移填洞。
				// 为什么不用墓碑:墓碑会让表随时间退化(每次探测都穿过它们),
				// 而这个表是长期运行的诊断设施,退化会直接变成帧时间抖动。
				size_t hole = index;
				size_t scan = (hole + 1) & state.Mask;
				while (state.Slots[scan].Key != 0)
				{
					const size_t home = ProbeStart(state.Slots[scan].Key, state.Capacity);
					// home 是否落在循环区间 (hole, scan] 内?落在外面的可以回移。
					const bool movable = (hole <= scan)
						? (home > hole && home <= scan)
						: (home > hole || home <= scan);
					if (!movable)
					{
						std::memcpy(&state.Slots[hole], &state.Slots[scan], sizeof(Slot));
						hole = scan;
					}
					scan = (scan + 1) & state.Mask;
				}
				std::memset(&state.Slots[hole], 0, sizeof(Slot));
				lock.Unlock();
				return true;
			}
			index = (index + 1) & state.Mask;
		}

		lock.Unlock();
		return false;
	}
	// ---------------------------------------------------------------------------
	// 分配器(池)级
	// ---------------------------------------------------------------------------

	uint32_t MemoryTrack::AcquireSite(void* origin) noexcept
	{
		if (!origin)
			return kNoSite;
		MemoryTrackState& state = State();
		const uint32_t count = state.SiteCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < count; ++i)
		{
			if (state.Sites[i].Origin.load(std::memory_order_relaxed) == origin)
				return i;
		}
		if (count >= MemoryTrackState::kMaxSites)
			return kNoSite;
		state.Sites[count].Origin.store(origin, std::memory_order_relaxed);
		state.SiteCount.store(count + 1, std::memory_order_release);
		return count;
	}

	void MemoryTrack::RegisterAllocator(Allocator* allocator) noexcept
	{
		if (!allocator)
			return;
		MemoryTrackState& state = State();
		std::lock_guard<std::mutex> guard(state.AllocatorMutex);
		const uint32_t count = state.AllocatorCount.load(std::memory_order_relaxed);
		if (count >= kMaxAllocators)
			return;
		state.Allocators[count].Ptr.store(allocator, std::memory_order_relaxed);
		state.Allocators[count].PeakBytes.store(0, std::memory_order_relaxed);
		state.AllocatorCount.store(count + 1, std::memory_order_release);
	}

	void MemoryTrack::UnregisterAllocator(Allocator* allocator) noexcept
	{
		if (!allocator)
			return;
		MemoryTrackState& state = State();
		std::lock_guard<std::mutex> guard(state.AllocatorMutex);
		const uint32_t count = state.AllocatorCount.load(std::memory_order_relaxed);
		for (uint32_t i = 0; i < count; ++i)
		{
			if (state.Allocators[i].Ptr.load(std::memory_order_relaxed) != allocator)
				continue;
			// **快照峰值后清零**,而不是移动表尾覆盖:分配器析构时其它线程可能正在 Tick,
			// 覆盖会让"正在读的那个槽"指向已销毁对象。
			state.Allocators[i].PeakBytes.store(0, std::memory_order_relaxed);
			state.Allocators[i].Ptr.store(nullptr, std::memory_order_release);
			return;
		}
	}

	// ---------------------------------------------------------------------------
	// GPU 驻留
	// ---------------------------------------------------------------------------

	namespace
	{
		GpuSlot* FindOrCreateGpuSlot(MemoryTrackState& state, const char* owner) noexcept
		{
			if (!owner || owner[0] == '\0')
				owner = "<gpu>";
			const uint32_t count = state.GpuCount.load(std::memory_order_acquire);
			for (uint32_t i = 0; i < count; ++i)
			{
				const char* existingOwner = state.Gpu[i].Name.load(std::memory_order_relaxed);
				// 同标签:指针快路径 + 内容回退(不同 TU 的字面量指针不同)。
				if (existingOwner == owner || (existingOwner && std::strcmp(existingOwner, owner) == 0))
					return &state.Gpu[i];
			}
			if (count >= MemoryTrack::kMaxGpuOwners)
				return nullptr;
			state.Gpu[count].Name.store(owner, std::memory_order_relaxed);
			state.GpuCount.store(count + 1, std::memory_order_release);
			return &state.Gpu[count];
		}
	}

	void MemoryTrack::AddGpuResident(const char* owner, uint64_t bytes) noexcept
	{
		MemoryTrackState& state = State();
		if (!state.Enabled || bytes == 0)
			return;
		if (GpuSlot* slot = FindOrCreateGpuSlot(state, owner))
		{
			slot->Bytes.fetch_add(bytes, std::memory_order_relaxed);
			slot->LiveCount.fetch_add(1, std::memory_order_relaxed);
		}
		state.GpuTotal.fetch_add(bytes, std::memory_order_relaxed);
	}

	void MemoryTrack::RemoveGpuResident(const char* owner, uint64_t bytes) noexcept
	{
		MemoryTrackState& state = State();
		if (!state.Enabled || bytes == 0)
			return;
		if (GpuSlot* slot = FindOrCreateGpuSlot(state, owner))
		{
			// 夹到 0:释放多于登记说明簿记错了,不能出现负值(会污染总量)。
			const uint64_t current = slot->Bytes.load(std::memory_order_relaxed);
			slot->Bytes.store(current >= bytes ? current - bytes : 0, std::memory_order_relaxed);
			const uint64_t count = slot->LiveCount.load(std::memory_order_relaxed);
			if (count > 0)
				slot->LiveCount.store(count - 1, std::memory_order_relaxed);
		}
		const uint64_t total = state.GpuTotal.load(std::memory_order_relaxed);
		state.GpuTotal.store(total >= bytes ? total - bytes : 0, std::memory_order_relaxed);
	}

	// ---------------------------------------------------------------------------
	// 每帧驱动
	// ---------------------------------------------------------------------------

	void MemoryTrack::Tick()
	{
		MemoryTrackState& state = State();
		if (!state.Enabled)
			return;

		// 池级峰值:每帧采样 ⇒ 面板不开也有真实峰值(旧实现的缺陷 A)。
		{
			std::lock_guard<std::mutex> guard(state.AllocatorMutex);
			const uint32_t count = state.AllocatorCount.load(std::memory_order_relaxed);
			for (uint32_t i = 0; i < count; ++i)
			{
				Allocator* allocator = state.Allocators[i].Ptr.load(std::memory_order_acquire);
				if (!allocator)
					continue;
				const uint64_t used = allocator->GetUsedMemory();
				const uint64_t current = state.Allocators[i].PeakBytes.load(std::memory_order_relaxed);
				if (used > current)
					state.Allocators[i].PeakBytes.store(used, std::memory_order_relaxed);
			}
		}

		// 标签峰值。
		const uint32_t tagCount = state.TagCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < tagCount; ++i)
		{
			const uint64_t live = state.Tags[i].LiveBytes.load(std::memory_order_relaxed);
			const uint64_t peak = state.Tags[i].PeakBytes.load(std::memory_order_relaxed);
			if (live > peak)
				state.Tags[i].PeakBytes.store(live, std::memory_order_relaxed);
		}

		// 总量峰值 + 帧趋势。
		const uint64_t live = state.HeapLive.load(std::memory_order_relaxed);
		const uint64_t peak = state.HeapPeak.load(std::memory_order_relaxed);
		if (live > peak)
			state.HeapPeak.store(live, std::memory_order_relaxed);

		state.TrendLive[state.TrendWrite] = live;
		state.TrendGpu[state.TrendWrite] = state.GpuTotal.load(std::memory_order_relaxed);
		state.TrendWrite = (state.TrendWrite + 1) % kMaxTrendFrames;
		if (state.TrendCount < kMaxTrendFrames)
			++state.TrendCount;
	}

	// ---------------------------------------------------------------------------
	// 查询
	// ---------------------------------------------------------------------------

	size_t MemoryTrack::CollectAllocators(AllocatorStat* out, size_t capacity)
	{
		MemoryTrackState& state = State();
		std::lock_guard<std::mutex> guard(state.AllocatorMutex);
		size_t written = 0;
		const uint32_t count = state.AllocatorCount.load(std::memory_order_relaxed);
		for (uint32_t i = 0; i < count && written < capacity; ++i)
		{
			Allocator* allocator = state.Allocators[i].Ptr.load(std::memory_order_acquire);
			if (!allocator)
				continue;
			AllocatorStat& stat = out[written++];
			stat.Name = allocator->GetDebugName();
			stat.UsedBytes = allocator->GetUsedMemory();
			stat.TotalReserved = allocator->GetSize();
			stat.NumAllocations = allocator->GetNumAllocations();
			stat.PeakBytes = state.Allocators[i].PeakBytes.load(std::memory_order_relaxed);
		}
		return written;
	}

	size_t MemoryTrack::CollectTags(TagStat* out, size_t capacity)
	{
		MemoryTrackState& state = State();
		size_t written = 0;
		const uint32_t count = state.TagCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < count && written < capacity; ++i)
		{
			const char* name = state.Tags[i].Name.load(std::memory_order_relaxed);
			if (!name)
				continue;
			TagStat& stat = out[written++];
			stat.Name = name;
			stat.LiveBytes = state.Tags[i].LiveBytes.load(std::memory_order_relaxed);
			stat.PeakBytes = state.Tags[i].PeakBytes.load(std::memory_order_relaxed);
			stat.LiveCount = state.Tags[i].LiveCount.load(std::memory_order_relaxed);
			stat.TotalAllocs = state.Tags[i].TotalAllocs.load(std::memory_order_relaxed);
			stat.TotalFrees = state.Tags[i].TotalFrees.load(std::memory_order_relaxed);
		}
		return written;
	}

	size_t MemoryTrack::CollectGpuOwners(GpuOwnerStat* out, size_t capacity)
	{
		MemoryTrackState& state = State();
		size_t written = 0;
		const uint32_t count = state.GpuCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < count && written < capacity; ++i)
		{
			const char* name = state.Gpu[i].Name.load(std::memory_order_relaxed);
			if (!name)
				continue;
			GpuOwnerStat& stat = out[written++];
			stat.Name = name;
			stat.Bytes = state.Gpu[i].Bytes.load(std::memory_order_relaxed);
			stat.LiveCount = state.Gpu[i].LiveCount.load(std::memory_order_relaxed);
		}
		return written;
	}

	size_t MemoryTrack::CollectLeaks(LeakEntry* out, size_t capacity)
	{
		MemoryTrackState& state = State();
		size_t written = 0;
		// 单趟扫表。这里刻意**不**逐分片加锁:泄漏枚举是采集结束后的诊断快照,
		// 逐分片加锁会把复杂度变成 O(分片数 × 容量)(64 × 524288 = 3300 万次比较)。
		// 代价是快照可能与并发写入轻微不一致 —— 诊断场景可接受,且不会崩(键是原子读)。
		for (uint32_t index = 0; index < state.Capacity && written < capacity; ++index)
		{
			const Slot& slot = state.Slots[index];
			if (slot.Key == 0)
				continue;
			LeakEntry& entry = out[written++];
			entry.Address = reinterpret_cast<void*>(slot.Key);
			entry.Size = slot.Size;
			entry.Tag = slot.Tag != 0
				? state.Tags[slot.Tag - 1].Name.load(std::memory_order_relaxed)
				: nullptr;
		}
		return written;
	}
	size_t MemoryTrack::CollectSites(SiteStat* out, size_t capacity)
	{
		MemoryTrackState& state = State();
		const uint32_t count = state.SiteCount.load(std::memory_order_acquire);
		size_t written = 0;
		for (uint32_t i = 0; i < count && written < capacity; ++i)
		{
			SiteStat& stat = out[written++];
			stat.Origin = state.Sites[i].Origin.load(std::memory_order_relaxed);
			stat.LiveBytes = state.Sites[i].LiveBytes.load(std::memory_order_relaxed);
			stat.LiveCount = state.Sites[i].LiveCount.load(std::memory_order_relaxed);
			stat.TotalAllocs = state.Sites[i].TotalAllocs.load(std::memory_order_relaxed);
		}
		// 按活跃字节降序:容量不足时丢掉的是"更不重要"的站点。
		std::sort(out, out + written, [](const SiteStat& a, const SiteStat& b)
			{
				if (a.LiveBytes != b.LiveBytes)
					return a.LiveBytes > b.LiveBytes;
				return a.LiveCount > b.LiveCount;
			});
		return written;
	}

	uint32_t MemoryTrack::SiteSamplingRate() noexcept
	{
		return State().SiteSampling.load(std::memory_order_relaxed);
	}

	size_t MemoryTrack::DumpReport(const char* path, size_t topSites)
	{
		if (!path || path[0] == 0)
			return 0;

		// 符号化会产生分配 ⇒ 抑制自身,避免污染被观察对象(这是我们在修的老毛病)。
		SymbolResolver::SuppressionScope suppress;

		std::ofstream stream(path, std::ios::trunc);
		if (!stream)
			return 0;

		size_t lines = 0;
		char buffer[1024];

		const uint32_t sampling = SiteSamplingRate();
		std::snprintf(buffer, sizeof(buffer),
			"# MemoryTrack report\n"
			"heapLive=%llu heapPeak=%llu liveAllocs=%llu pools=%llu gpuResident=%llu "
			"untrackedFrees=%llu siteSampling=1/%u insertFailures=%llu\n",
			static_cast<unsigned long long>(HeapLiveBytes()),
			static_cast<unsigned long long>(HeapPeakBytes()),
			static_cast<unsigned long long>(LiveAllocationCount()),
			static_cast<unsigned long long>(PoolUsedBytes()),
			static_cast<unsigned long long>(GpuResidentBytes()),
			static_cast<unsigned long long>(UnknownFrees()),
			sampling,
			static_cast<unsigned long long>(State().InsertFailures.load(std::memory_order_relaxed)));
		stream << buffer;
		++lines;

		// Top 站点(按活跃字节)—— 回答"哪一行在分配"。
		std::vector<SiteStat> sites(static_cast<size_t>(State().SiteCount.load(std::memory_order_relaxed)) + 1);
		const size_t siteCount = CollectSites(sites.data(), sites.size());
		stream << "\n# top live call sites (1/" << sampling << " sampled)\n";
		for (size_t i = 0; i < siteCount && i < topSites; ++i)
		{
			std::snprintf(buffer, sizeof(buffer), "%10llu B  x%-6llu  %s\n",
				static_cast<unsigned long long>(sites[i].LiveBytes),
				static_cast<unsigned long long>(sites[i].LiveCount),
				SymbolResolver::Describe(sites[i].Origin).c_str());
			stream << buffer;
			++lines;
		}

		// 活跃分配的大小直方图:快速判断"是很多小分配,还是少数大块"。
		std::vector<LeakEntry> all(65536);
		const size_t leakCount = CollectLeaks(all.data(), all.size());
		{
			struct Bucket { const char* Label; uint64_t Count; uint64_t Bytes; };
			Bucket buckets[] = {
				{ "<=64B", 0, 0 }, { "<=256B", 0, 0 }, { "<=1KB", 0, 0 },
				{ "<=16KB", 0, 0 }, { "<=1MB", 0, 0 }, { ">1MB", 0, 0 },
			};
			for (size_t i = 0; i < leakCount; ++i)
			{
				const uint64_t size = all[i].Size;
				const size_t bucket = size <= 64 ? 0 : size <= 256 ? 1 : size <= 1024 ? 2
					: size <= 16 * 1024 ? 3 : size <= 1024 * 1024 ? 4 : 5;
				buckets[bucket].Count += 1;
				buckets[bucket].Bytes += size;
			}
			stream << "\n# live allocation size histogram\n";
			for (const Bucket& bucket : buckets)
			{
				std::snprintf(buffer, sizeof(buffer), "%-8s count=%-8llu bytes=%llu\n",
					bucket.Label,
					static_cast<unsigned long long>(bucket.Count),
					static_cast<unsigned long long>(bucket.Bytes));
				stream << buffer;
				++lines;
			}
		}

		// Top 活跃分配(按字节)+ 其采样调用点。
		std::sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(leakCount),
			[](const LeakEntry& a, const LeakEntry& b) { return a.Size > b.Size; });
		stream << "\n# top live allocations by size\n";
		for (size_t i = 0; i < leakCount && i < topSites; ++i)
		{
			std::snprintf(buffer, sizeof(buffer), "%10llu B  tag=%s\n",
				static_cast<unsigned long long>(all[i].Size), all[i].Tag ? all[i].Tag : "-");
			stream << buffer;
			++lines;
			if (all[i].Origin)
			{
				stream << "              " << SymbolResolver::Describe(all[i].Origin) << "\n";
				++lines;
			}
		}

		stream.flush();
		WLD_CORE_INFO("[memory] dump written: {0} ({1} lines)", path, lines);
		return lines;
	}

	size_t MemoryTrack::CollectTrend(uint64_t* liveBytes, uint64_t* gpuBytes, size_t capacity)
	{
		MemoryTrackState& state = State();
		const size_t count = std::min<size_t>(state.TrendCount, capacity);
		// 返回**最近** count 个样本(最旧在前、最新在后)。
		//
		// 修(2026-10-06):原实现从 `TrendWrite` 起取 count 个 —— 环形写满后
		// `TrendWrite` 指向**最旧**槽,于是返回的是**最旧**的 count 个样本。
		// 消费者(内存面板)请求 600 帧而环形有 3600 帧 ⇒ 面板前 10 秒正常,
		// 之后永久冻结在第 0..599 帧上 —— 表现为"曲线有时候不动了"。
		//
		// `TrendWrite` 在未写满时 == 已写入数,写满后 == 最旧槽;两种情况统一为
		// "从 (write - count) 开始"。
		const size_t start = (state.TrendWrite + kMaxTrendFrames - count) % kMaxTrendFrames;
		for (size_t i = 0; i < count; ++i)
		{
			const size_t index = (start + i) % kMaxTrendFrames;
			if (liveBytes)
				liveBytes[i] = state.TrendLive[index];
			if (gpuBytes)
				gpuBytes[i] = state.TrendGpu[index];
		}
		return count;
	}

	uint64_t MemoryTrack::HeapLiveBytes() { return State().HeapLive.load(std::memory_order_relaxed); }
	uint64_t MemoryTrack::HeapPeakBytes() { return State().HeapPeak.load(std::memory_order_relaxed); }
	uint64_t MemoryTrack::GpuResidentBytes() { return State().GpuTotal.load(std::memory_order_relaxed); }
	uint64_t MemoryTrack::LiveAllocationCount() { return State().HeapLiveCount.load(std::memory_order_relaxed); }
	uint64_t MemoryTrack::UnknownFrees() { return State().UnknownFrees.load(std::memory_order_relaxed); }

	uint64_t MemoryTrack::PoolUsedBytes()
	{
		MemoryTrackState& state = State();
		std::lock_guard<std::mutex> guard(state.AllocatorMutex);
		uint64_t total = 0;
		const uint32_t count = state.AllocatorCount.load(std::memory_order_relaxed);
		for (uint32_t i = 0; i < count; ++i)
		{
			if (Allocator* allocator = state.Allocators[i].Ptr.load(std::memory_order_acquire))
				total += allocator->GetUsedMemory();
		}
		return total;
	}

	std::string MemoryTrack::DescribeText()
	{
		char buffer[512];
		std::snprintf(buffer, sizeof(buffer),
			"Memory: heap live %.2f MB (peak %.2f MB, %llu allocs live) | pools %.2f MB | GPU %.2f MB | untracked frees %llu\n",
			static_cast<double>(HeapLiveBytes()) / (1024.0 * 1024.0),
			static_cast<double>(HeapPeakBytes()) / (1024.0 * 1024.0),
			static_cast<unsigned long long>(LiveAllocationCount()),
			static_cast<double>(PoolUsedBytes()) / (1024.0 * 1024.0),
			static_cast<double>(GpuResidentBytes()) / (1024.0 * 1024.0),
			static_cast<unsigned long long>(UnknownFrees()));
		return buffer;
	}

	std::string MemoryTrack::DescribeJson()
	{
		TagStat tags[16];
		const size_t tagCount = CollectTags(tags, 16);
		GpuOwnerStat gpu[8];
		const size_t gpuCount = CollectGpuOwners(gpu, 8);

		std::string out;
		out.reserve(1024);
		char buffer[512];
		std::snprintf(buffer, sizeof(buffer),
			"{\"enabled\":%s,\"heapLive\":%llu,\"heapPeak\":%llu,\"liveAllocs\":%llu,"
			"\"pools\":%llu,\"gpuResident\":%llu,\"untrackedFrees\":%llu,\"tags\":[",
			Enabled() ? "true" : "false",
			static_cast<unsigned long long>(HeapLiveBytes()),
			static_cast<unsigned long long>(HeapPeakBytes()),
			static_cast<unsigned long long>(LiveAllocationCount()),
			static_cast<unsigned long long>(PoolUsedBytes()),
			static_cast<unsigned long long>(GpuResidentBytes()),
			static_cast<unsigned long long>(UnknownFrees()));
		out += buffer;

		for (size_t i = 0; i < tagCount; ++i)
		{
			std::snprintf(buffer, sizeof(buffer),
				"%s{\"name\":\"%s\",\"live\":%llu,\"peak\":%llu,\"allocs\":%llu}",
				i == 0 ? "" : ",", tags[i].Name ? tags[i].Name : "",
				static_cast<unsigned long long>(tags[i].LiveBytes),
				static_cast<unsigned long long>(tags[i].PeakBytes),
				static_cast<unsigned long long>(tags[i].LiveCount));
			out += buffer;
		}
		out += "],\"gpu\":[";
		for (size_t i = 0; i < gpuCount; ++i)
		{
			std::snprintf(buffer, sizeof(buffer),
				"%s{\"name\":\"%s\",\"bytes\":%llu,\"count\":%llu}",
				i == 0 ? "" : ",", gpu[i].Name ? gpu[i].Name : "",
				static_cast<unsigned long long>(gpu[i].Bytes),
				static_cast<unsigned long long>(gpu[i].LiveCount));
			out += buffer;
		}
		out += "]}";
		return out;
	}
}
