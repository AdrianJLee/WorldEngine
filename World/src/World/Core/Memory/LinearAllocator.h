#pragma once
#include "Memory.h"
namespace World
{
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