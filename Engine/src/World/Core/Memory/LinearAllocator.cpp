#include "wldpch.h"
#include "LinearAllocator.h"

namespace World
{
	LinearAllocator::LinearAllocator(size_t size, void* start, const char* debugName)
		: Allocator(size, start, debugName, AllocatorType::Linear), m_CurrentPos(start)
	{}

	void* LinearAllocator::Allocate(size_t size, size_t alignment)
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
	void LinearAllocator::Reset()
	{
		m_CurrentPos = m_Start;
		m_UsedMemory = 0;
		m_NumAllocations = 0;
	}
}