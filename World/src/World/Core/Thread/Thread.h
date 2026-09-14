#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace World
{
	// 任务优先级:高优先级的任务先被本线程取走,低优先级更容易被其它线程偷走。
	enum class JobPriority : uint8_t
	{
		Low = 0,
		Normal = 1,
		High = 2,
		Count = 3,
	};

	// 任务计数器:提交任务时自增,完成时自减;归零即"全部完成"。
	// Cancel() 只影响尚未开始执行的任务(已开始的会跑完),用于关卡卸载/取消长任务。
	struct JobCounter
	{
		std::atomic<int> Count { 0 };
		std::atomic<bool> Cancelled { false };

		bool IsComplete() const { return Count.load(std::memory_order_acquire) == 0; }
		void Cancel() { Cancelled.store(true, std::memory_order_release); }
		bool IsCancelled() const { return Cancelled.load(std::memory_order_acquire); }
	};

	// 任务负载的内联容量。超过则自动落到堆上(Emplace 内部处理),
	// 不再像旧实现那样"静默丢弃大负载"。
	inline constexpr uint32_t JOB_INLINE_SIZE = 128;

	// 任务声明:函数指针 + 负载 + 计数器。
	struct JobDecl
	{
		alignas(16) uint8_t Storage[JOB_INLINE_SIZE] {};
		void (*Entry)(void*) = nullptr;
		void (*Destroy)(void*) = nullptr;   // 负载需要析构时非空(内联与堆通用)
		void* HeapPayload = nullptr;        // 超出内联容量时的堆负载
		size_t HeapAlignment = 16;          // 堆负载的对齐(释放时必须一致)
		JobCounter* Counter = nullptr;
		JobPriority Priority = JobPriority::Normal;

		JobDecl() = default;

		void* Data() { return HeapPayload ? HeapPayload : Storage; }
		const void* Data() const { return HeapPayload ? HeapPayload : Storage; }

		template <typename T>
		void Emplace(T&& data)
		{
			using Payload = std::decay_t<T>;
			Release();
			if constexpr (sizeof(Payload) <= JOB_INLINE_SIZE)
			{
				new (Storage) Payload(std::forward<T>(data));
				SetDestroyer<Payload>();
			}
			else
			{
				HeapPayload = ::operator new(sizeof(Payload), std::align_val_t { alignof(Payload) });
				HeapAlignment = alignof(Payload);
				new (HeapPayload) Payload(std::forward<T>(data));
				SetDestroyer<Payload>();
			}
		}

		// 任务执行完毕(或取消)后释放负载。
		void Release()
		{
			if (Destroy)
			{
				Destroy(Data());
				Destroy = nullptr;
			}
			if (HeapPayload)
			{
				::operator delete(HeapPayload, std::align_val_t { HeapAlignment });
				HeapPayload = nullptr;
				HeapAlignment = 16;
			}
			Entry = nullptr;
			Priority = JobPriority::Normal;
		}

	private:
		template <typename Payload>
		void SetDestroyer()
		{
			if constexpr (std::is_trivially_destructible_v<Payload>)
				Destroy = nullptr;
			else
				Destroy = [](void* memory) { static_cast<Payload*>(memory)->~Payload(); };
		}
	};
}
