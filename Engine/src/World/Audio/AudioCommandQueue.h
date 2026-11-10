#pragma once

#include "AudioTypes.h"
#include <atomic>
#include <array>
#include <cstddef>
#include <type_traits>

namespace World::Audio
{
	template<typename T, size_t Capacity = 1024>
	class SpscQueue
	{
		static_assert((Capacity > 0) && ((Capacity & (Capacity - 1)) == 0), "Capacity must be a power of two");
		static_assert(std::is_trivially_copyable_v<T> || std::is_copy_constructible_v<T>, "T must be copyable");

	public:
		SpscQueue() = default;
		~SpscQueue() = default;

		SpscQueue(const SpscQueue&) = delete;
		SpscQueue& operator=(const SpscQueue&) = delete;
		SpscQueue(SpscQueue&&) = delete;
		SpscQueue& operator=(SpscQueue&&) = delete;

		bool Push(const T& item)
		{
			const size_t head = m_Head.load(std::memory_order_relaxed);
			const size_t tail = m_Tail.load(std::memory_order_acquire);
			if (head - tail >= Capacity)
			{
				return false; // Queue is full
			}
			m_Buffer[head & (Capacity - 1)] = item;
			m_Head.store(head + 1, std::memory_order_release);
			return true;
		}

		bool Pop(T& item)
		{
			const size_t tail = m_Tail.load(std::memory_order_relaxed);
			const size_t head = m_Head.load(std::memory_order_acquire);
			if (tail == head)
			{
				return false; // Queue is empty
			}
			item = m_Buffer[tail & (Capacity - 1)];
			m_Tail.store(tail + 1, std::memory_order_release);
			return true;
		}

		bool IsEmpty() const noexcept
		{
			return m_Tail.load(std::memory_order_relaxed) == m_Head.load(std::memory_order_relaxed);
		}

		size_t Size() const noexcept
		{
			const size_t head = m_Head.load(std::memory_order_relaxed);
			const size_t tail = m_Tail.load(std::memory_order_relaxed);
			return (head >= tail) ? (head - tail) : 0;
		}

		static constexpr size_t GetCapacity() noexcept
		{
			return Capacity;
		}

		void Clear() noexcept
		{
			m_Tail.store(m_Head.load(std::memory_order_relaxed), std::memory_order_release);
		}

	private:
		alignas(64) std::atomic<size_t> m_Head{0};
		alignas(64) std::atomic<size_t> m_Tail{0};
		alignas(64) std::array<T, Capacity> m_Buffer{};
	};

	using AudioCommandQueue = SpscQueue<AudioCommand, 1024>;
}
