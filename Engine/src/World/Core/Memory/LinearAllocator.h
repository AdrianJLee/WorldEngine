#pragma once
#include "Allocator.h"

namespace World
{
	// 线性分配器（Linear Allocator）适用于一次性分配大量内存的场景，分配速度非常快，但不支持单独释放内存块，只能通过 Reset() 一次性清空所有分配
	// Linear, 极其简单的线性增长, 极短(1帧), 性能之王，不支持回滚或单个释放
	class LinearAllocator : public Allocator
	{
	public:
		LinearAllocator(size_t size, void* start, const char* debugName);

		~LinearAllocator() override = default;

		void* Allocate(size_t size, size_t alignment = 8) override;

		void Reset();

	private:
		void* m_CurrentPos;
	};
}