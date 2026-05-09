#include "wldpch.h"
#include "GrowableLinearAllocator.h"

namespace World
{
	GrowableLinearAllocator::GrowableLinearAllocator(size_t pageSize, const char* debugName)
		: Allocator(0, nullptr, debugName, AllocatorType::Linear), m_PageSize(pageSize)
	{
		m_CurrentPage = CreatePage(m_PageSize);
		m_HeadPage = m_CurrentPage;
	}

	GrowableLinearAllocator::~GrowableLinearAllocator()
	{
		// 析构时释放所有页
		MemoryPage* p = m_HeadPage;
		while (p)
		{
			MemoryPage* next = p->Next;
			free(p->Data);
			delete p;
			p = next;
		}
		m_HeadPage = nullptr;
		m_CurrentPage = nullptr;
	}

	void* GrowableLinearAllocator::Allocate(size_t size, size_t alignment)
	{
		uintptr_t currentAddr = reinterpret_cast<uintptr_t>(m_CurrentPage->Data) + m_CurrentPage->Offset;
		uintptr_t alignedAddr = AlignForward(currentAddr, alignment);

		// 如果当前页放不下了
		if (alignedAddr + size > reinterpret_cast<uintptr_t>(m_CurrentPage->Data) + m_CurrentPage->Size)
		{
			// 申请新页（大小至少能放下本次请求，或者是默认页大小）
			size_t nextSize = std::max(m_PageSize, size + alignment);
			MemoryPage* newPage = CreatePage(nextSize);

			// 链接并切换
			m_CurrentPage->Next = newPage;
			m_CurrentPage = newPage;

			// 在新页重新计算对齐地址
			alignedAddr = AlignForward(reinterpret_cast<uintptr_t>(m_CurrentPage->Data), alignment);
		}

		m_CurrentPage->Offset = (alignedAddr + size) - reinterpret_cast<uintptr_t>(m_CurrentPage->Data);
		m_UsedMemory += size; // 统计总占用
		return reinterpret_cast<void*>(alignedAddr);
	}
	void GrowableLinearAllocator::Reset()
	{
		// 重置时不需要释放内存，只需把所有 Page 的 Offset 归零
		// 只有在析构时才真正 free
		MemoryPage* p = m_HeadPage;
		while (p)
		{
			p->Offset = 0;
			p = p->Next;
		}
		m_CurrentPage = m_HeadPage;
		m_UsedMemory = 0;
	}

	MemoryPage* GrowableLinearAllocator::CreatePage(size_t size)
	{
		void* data = malloc(size);
		return new MemoryPage { data, size, 0, nullptr };
	}

}