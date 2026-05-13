#include "wldpch.h"
#include "StackAllocator.h"
#include "MemoryTracker.h"
namespace World
{
	StackAllocator::StackAllocator(size_t size, void* start, const char* debugName, bool isEphemeral)
		: Allocator(size, start, debugName, AllocatorType::Stack, isEphemeral), m_CurrentPos(start)
	{
		//WLD_CORE_INFO("Stack Allocator Created: Size = {} bytes, Start = {}", size, start);
	}

	StackAllocator::~StackAllocator()
	{
		//WLD_CORE_INFO("Stack Allocator Destroyed: Start = {}", m_Start);
		MemoryTracker::Get().AddEphemeralSnapshot(this);
		Clear();
		m_Start = nullptr;
		m_CurrentPos = nullptr;
		m_DestructorChain = nullptr;
	}

	void* StackAllocator::Allocate(size_t size, size_t alignment)
	{
		uintptr_t currentAddr = reinterpret_cast<uintptr_t>(m_CurrentPos);
		uintptr_t alignedAddr = AlignForward(currentAddr, alignment);

		if (alignedAddr + size > reinterpret_cast<uintptr_t>(m_Start) + m_Size)
		{
			WLD_CORE_ASSERT(false, "Stack Allocator Overflow!");
			return nullptr;
		}

		m_CurrentPos = reinterpret_cast<void*>(alignedAddr + size);

		// 更新基类统计信息
		m_UsedMemory = (uintptr_t)m_CurrentPos - (uintptr_t)m_Start;
		m_NumAllocations++;

		return reinterpret_cast<void*>(alignedAddr);
	}

	void StackAllocator::FreeToMarker(Marker marker)
	{
		uintptr_t targetAddr = (uintptr_t)m_Start + marker;

		if (m_DestructorChain)
		{
			// 遍历并调用回滚区域内的析构函数
			while ((uintptr_t)m_DestructorChain->Object >= targetAddr)
			{
				m_DestructorChain->Callback(m_DestructorChain->Object); // 执行析构

				if (m_NumAllocations >= 2)
					m_NumAllocations -= 2;

				m_DestructorChain = m_DestructorChain->Next;
			}
		}
		m_CurrentPos = reinterpret_cast<void*>(targetAddr);
		m_UsedMemory = marker;

		//WLD_CORE_INFO("Stack Allocator FreeToMarker: Freed to marker {}, Current Used Memory = {} bytes", marker, m_UsedMemory);
	}

	void StackAllocator::Clear()
	{
		FreeToMarker(0);
		m_NumAllocations = 0;
	}

	void StackAllocator::RegisterDestructor(void* obj, DestructorFunc func)
	{
		// 在栈上直接分配一个节点空间
		void* nodeMem = this->Allocate(sizeof(DestructorNode), alignof(DestructorNode));
		DestructorNode* node = new (nodeMem) DestructorNode { func, obj, m_DestructorChain };
		m_DestructorChain = node;
	}

	ScopedStack::ScopedStack(size_t size, const char* debugName, bool isEphemeral)
		: m_RawMemory(_aligned_malloc(size, 16)),
		m_Allocator(size, m_RawMemory, debugName, isEphemeral)
	{
		//WLD_CORE_INFO("ScopedStack Created: Size = {} bytes, Raw Memory = {}", size, m_RawMemory);
	}

	ScopedStack::~ScopedStack()
	{
		//WLD_CORE_INFO("ScopedStack Destroyed: Raw Memory = {}", m_RawMemory);
		// 自动执行顺序：
		// 1. m_Allocator 的析构函数会自动运行（清理析构链）
		// 2. 然后进入 ScopedStack 析构函数体，执行物理内存释放
		if (m_RawMemory)
		{

			_aligned_free(m_RawMemory);
			m_RawMemory = nullptr;
		}
	}
}