#pragma once

#include "World/Profiling/TraceEvents.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace World::Profiling
{
	// ---------------------------------------------------------------------------
	// 采集缓冲:一次性预分配的事件数组 + 原子槽位分配。
	//
	// 设计取舍(为什么不是"每线程环形缓冲"):
	//   * 事件槽位由一次 `fetch_add` 分配 ⇒ **无锁、无共享写游标**,任意线程可写;
	//   * 不覆写、不回卷 ⇒ 采集窗口内的数据**完整**,"丢"只发生在容量耗尽时,
	//     且必须计数(kDropped 可见),不允许静默截断;
	//   * 分配只发生在**采集开始时一次**(不在帧循环内),因此"每帧堆分配 0"仍成立。
	//
	// 线程契约(重要):
	//   Push 可从任意线程调用;读取(Data/Size/Reset)**只允许在所有写者停下之后**进行
	//   —— 由 Telemetry 保证:停止采集后先 JobSystem::WaitAll(),再读。
	// ---------------------------------------------------------------------------
	class TraceBuffer
	{
	public:
		// 524288 × 40B = 20 MiB 上限。
		static constexpr uint32_t kDefaultCapacity = 1u << 19;
		// 4096 × 40B = 160 KiB:测试与低开销场景用。
		static constexpr uint32_t kSmallCapacity = 1u << 12;

		// 采集开始时调用一次(不在帧循环内,故允许分配)。已分配且容量足够时复用。
		bool EnsureAllocated(uint32_t capacity)
		{
			if (m_Events && m_Capacity >= capacity)
				return true;
			std::unique_ptr<TraceEvent[]> storage(new (std::nothrow) TraceEvent[capacity]);
			if (!storage)
				return false;
			m_Events = std::move(storage);
			m_Capacity = capacity;
			Reset();
			return true;
		}

		void Release()
		{
			m_Events.reset();
			m_Capacity = 0;
			Reset();
		}

		void Reset()
		{
			m_Write.store(0, std::memory_order_relaxed);
			m_Dropped.store(0, std::memory_order_relaxed);
		}

		// 热路径:无锁、无分配。容量耗尽时只自增丢弃计数。
		void Push(const TraceEvent& event) noexcept
		{
			const uint32_t slot = m_Write.fetch_add(1, std::memory_order_acq_rel);
			if (slot >= m_Capacity)
			{
				m_Dropped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			m_Events[slot] = event;
		}

		uint32_t Capacity() const noexcept { return m_Capacity; }
		uint32_t Size() const noexcept
		{
			const uint32_t written = m_Write.load(std::memory_order_acquire);
			return written < m_Capacity ? written : m_Capacity;
		}
		// 超过容量的写入次数(>0 ⇒ 本次采集不可用于定量结论)。
		uint64_t Dropped() const noexcept { return m_Dropped.load(std::memory_order_relaxed); }
		const TraceEvent* Data() const noexcept { return m_Events.get(); }
		size_t BytesInUse() const noexcept
		{
			return static_cast<size_t>(m_Capacity) * sizeof(TraceEvent);
		}

	private:
		std::unique_ptr<TraceEvent[]> m_Events;
		std::atomic<uint32_t> m_Write { 0 };
		std::atomic<uint64_t> m_Dropped { 0 };
		uint32_t m_Capacity = 0;
	};
}
