#pragma once

namespace World
{
	// 预留的内联负载大小（字节）。64字节对于一般的 Lambda 完全足够
	static constexpr uint32_t JOB_PADDING_SIZE = 64;

	// 任务计数器，用于跟踪正在执行的任务数量，支持原子操作以确保线程安全
	struct JobCounter
	{
		std::atomic<int> Count { 0 }; // 任务计数器，初始值为0
	};

	// JobDeclaration,任务声明结构体，包含任务函数指针和任务数据指针
	struct JobDecl
	{

		// 我们不再使用 `void* Data` 去指向外面的大对象池或者堆内存！
		// 取而代之，我们直接把闭包按位拍到这个内联数组里面！
		alignas(16) uint8_t Padding[JOB_PADDING_SIZE];

		void (*Entry)(void*); // 任务函数指针

		JobCounter* Counter = nullptr;

		JobDecl() : Entry(nullptr) {}

		// 极简包装：判断有没有越界
		template <typename T>
		void Emplace(T&& data)
		{
			static_assert(sizeof(T) <= JOB_PADDING_SIZE, "Job payload is too large to fit in inline padding!");
			static_assert(std::is_trivially_destructible_v<T>, "Job payload must not require a custom destructor!");

			// Placement new：直接在这个固定空间上原位构造对象
			new (Padding) T(std::forward<T>(data));
		}
	};



}