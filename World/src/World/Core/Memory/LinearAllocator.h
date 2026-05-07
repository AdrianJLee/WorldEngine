#pragma once
#include "Memory.h"
namespace World
{

	// 线性分配器（Linear Allocator）适用于一次性分配大量内存的场景，分配速度非常快，但不支持单独释放内存块，只能通过 Reset() 一次性清空所有分配
	// Linear, 极其简单的线性增长, 极短(1帧), 性能之王，不支持回滚或单个释放
	class LinearAllocator : public Allocator
	{
	public:
		LinearAllocator(size_t size, void* start)
			: Allocator(size, start), m_CurrentPos(start)
		{}

		void* Allocate(size_t size, size_t alignment = 8) override
		{
			uintptr_t currentAddr = reinterpret_cast<uintptr_t>(m_CurrentPos);
			uintptr_t alignedAddr = AlignForward(currentAddr, alignment);

			if (alignedAddr + size > reinterpret_cast<uintptr_t>(m_Start) + m_Size)
			{
				return nullptr; // 满了就返回空，交给上层管理者处理
			}

			m_CurrentPos = reinterpret_cast<void*>(alignedAddr + size);
			m_UsedMemory = (uintptr_t)m_CurrentPos - (uintptr_t)m_Start;
			m_NumAllocations++;
			return reinterpret_cast<void*>(alignedAddr);
		}

		void Deallocate(void* p) override { /* 线性分配器不单独释放 */ }

		void Reset()
		{
			m_CurrentPos = m_Start;
			m_UsedMemory = 0;
			m_NumAllocations = 0;
		}

	private:
		void* m_CurrentPos;
	};
}