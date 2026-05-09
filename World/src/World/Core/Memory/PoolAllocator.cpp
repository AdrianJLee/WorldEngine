#include "wldpch.h"
#include "PoolAllocator.h"
#include "MemoryTracker.h"
namespace World
{
	PoolAllocator::PoolAllocator(const char* debugName, PoolTag tag, size_t objectSize, size_t objectAlignment, size_t objectsPerChunk)
		: Allocator(0, nullptr, debugName, AllocatorType::Pool), m_Tag(tag), m_ObjectSize(std::max(objectSize, sizeof(Node))),
		m_Alignment(objectAlignment), m_ObjectsPerChunk(objectsPerChunk),
		m_FreeList(nullptr), m_ChunkList(nullptr)
	{}

	PoolAllocator::~PoolAllocator()
	{
		// 销毁所有申请的 Chunk
		Chunk* curr = m_ChunkList;
		while (curr)
		{
			Chunk* toDelete = curr;
			curr = curr->Next;
			_aligned_free(toDelete->Data);
			delete toDelete;
		}
	}

	void* PoolAllocator::Allocate(size_t size, size_t alignment)
	{
		WLD_CORE_ASSERT(size <= m_ObjectSize, "Allocation size too large for pool!");
		WLD_CORE_ASSERT(alignment <= m_Alignment, "Alignment requirement too strict!");

		// 如果 Free List 为空，说明需要申请新的 Chunk
		if (!m_FreeList)
		{
			Grow();
		}

		// 从 Free List 头部取出一个块
		Node* head = m_FreeList;
		m_FreeList = head->Next;

		m_UsedMemory += m_ObjectSize;
		m_NumAllocations++;
		return reinterpret_cast<void*>(head);
	}

	void PoolAllocator::Deallocate(void* p)
	{
		if (!p) return;

		// 将释放的内存块插回 Free List 头部
		Node* node = reinterpret_cast<Node*>(p);
		node->Next = m_FreeList;
		m_FreeList = node;

		m_UsedMemory -= m_ObjectSize;
		m_NumAllocations--;
	}
	void PoolAllocator::Grow()
	{
		size_t chunkMemorySize = m_ObjectSize * m_ObjectsPerChunk;
		void* raw = _aligned_malloc(chunkMemorySize, m_Alignment);

		// 统计总共从系统申请的内存（包含未使用的 Slots）
		m_Size += chunkMemorySize;

		// 将新申请的内存划分成若干 Node 链表
		for (size_t i = 0; i < m_ObjectsPerChunk; ++i)
		{
			Node* newNode = reinterpret_cast<Node*>(
				static_cast<uint8_t*>(raw) + (i * m_ObjectSize)
				);
			newNode->Next = m_FreeList;
			m_FreeList = newNode;
		}

		// 记录 Chunk 以便析构
		Chunk* newChunk = new Chunk { raw, m_ChunkList };
		m_ChunkList = newChunk;
	}
}