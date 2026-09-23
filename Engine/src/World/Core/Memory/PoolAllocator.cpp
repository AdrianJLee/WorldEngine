#include "wldpch.h"
#include "PoolAllocator.h"
#include "MemoryTracker.h"
namespace World
{
	namespace
	{
		constexpr size_t kCanarySize = 8;
		constexpr uint8_t kCanaryByte = 0xFD;
		constexpr uint8_t kFreeByte = 0xDD;

		void WriteCanary(void* block, size_t objectSize)
		{
			auto* bytes = static_cast<uint8_t*>(block);
			std::memset(bytes, kCanaryByte, kCanarySize);
			std::memset(bytes + kCanarySize + objectSize, kCanaryByte, kCanarySize);
		}

		bool CanaryIntact(const void* block, size_t objectSize)
		{
			const auto* bytes = static_cast<const uint8_t*>(block);
			for (size_t i = 0; i < kCanarySize; ++i)
				if (bytes[i] != kCanaryByte || bytes[kCanarySize + objectSize + i] != kCanaryByte)
					return false;
			return true;
		}
	}

	PoolAllocator::PoolAllocator(const char* debugName, PoolTag tag, size_t objectSize, size_t objectAlignment, size_t objectsPerChunk)
		: Allocator(0, nullptr, debugName, AllocatorType::Pool), m_Tag(tag), m_ObjectSize(std::max(objectSize, sizeof(Node))),
		m_Alignment(objectAlignment), m_ObjectsPerChunk(objectsPerChunk),
		m_FreeList(nullptr), m_ChunkList(nullptr)
	{
		m_Owner = std::this_thread::get_id();
#ifdef WLD_DEBUG
		// Debug 下每块前后各 8 字节哨兵:越界写在校验/释放时能被发现。
		m_BlockSize = kCanarySize + m_ObjectSize + kCanarySize;
#else
		m_BlockSize = m_ObjectSize;
#endif
		MemoryTracker::Get().Register(this, m_DebugName ? m_DebugName : "Pool", AllocatorType::Pool);
	}

	PoolAllocator::~PoolAllocator()
	{
		// 销毁所有申请的 Chunk
		Chunk* curr = m_ChunkList;
		while (curr)
		{
			Chunk* toDelete = curr;
			curr = curr->Next;

			m_Size -= (m_ObjectSize * m_ObjectsPerChunk);

			_aligned_free(toDelete->Data);
			delete toDelete;
		}
		m_UsedMemory = 0;
		m_NumAllocations = 0;
		MemoryTracker::Get().Unregister(this);
	}

	void* PoolAllocator::Allocate(size_t size, size_t alignment)
	{
		WLD_CORE_ASSERT(size <= m_ObjectSize, "Allocation size too large for pool!");
		WLD_CORE_ASSERT(alignment <= m_Alignment, "Alignment requirement too strict!");
		WLD_CORE_ASSERT(IsOwnerThread(), "PoolAllocator is thread-local; allocate/free from its owner thread only");

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
#ifdef WLD_DEBUG
		{
			auto* block = reinterpret_cast<uint8_t*>(head);
			WriteCanary(block, m_ObjectSize);
			return block + kCanarySize;
		}
#else
		return reinterpret_cast<void*>(head);
#endif
	}

	void PoolAllocator::Deallocate(void* p)
	{
		if (!p) return;
		WLD_CORE_ASSERT(IsOwnerThread(), "PoolAllocator is thread-local; allocate/free from its owner thread only");

#ifdef WLD_DEBUG
		uint8_t* block = static_cast<uint8_t*>(p) - kCanarySize;
		if (!CanaryIntact(block, m_ObjectSize))
		{
			++m_CorruptedBlocks;
			WLD_CORE_ERROR("[pool] buffer overrun detected in pool '{0}' (block {1})",
				m_DebugName ? m_DebugName : "Pool", static_cast<const void*>(p));
		}
		std::memset(p, kFreeByte, m_ObjectSize);
		Node* node = reinterpret_cast<Node*>(block);
#else
		// 将释放的内存块插回 Free List 头部
		Node* node = reinterpret_cast<Node*>(p);
#endif
		node->Next = m_FreeList;
		m_FreeList = node;

		m_UsedMemory -= m_ObjectSize;
		m_NumAllocations--;
	}

	size_t PoolAllocator::ValidateAllocations() const
	{
#ifndef WLD_DEBUG
		return 0;   // 未启用调试哨兵时无从校验
#else
		// 空闲块的前 8 字节被 free-list 指针复用,先把它们收集起来以便跳过。
		std::vector<const void*> freeBlocks;
		for (Node* node = m_FreeList; node; node = node->Next)
			freeBlocks.push_back(node);

		size_t corrupted = 0;
		for (Chunk* chunk = m_ChunkList; chunk; chunk = chunk->Next)
		{
			auto* base = static_cast<uint8_t*>(chunk->Data);
			for (size_t i = 0; i < m_ObjectsPerChunk; ++i)
			{
				uint8_t* block = base + i * m_BlockSize;
				if (std::find(freeBlocks.begin(), freeBlocks.end(), static_cast<const void*>(block)) != freeBlocks.end())
					continue;
				if (!CanaryIntact(block, m_ObjectSize))
					++corrupted;
			}
		}
		return corrupted;
#endif
	}

	size_t PoolAllocator::TrimFreeChunks()
	{
		size_t released = 0;
		Chunk* previous = nullptr;
		Chunk* chunk = m_ChunkList;
		while (chunk)
		{
			// 统计该 chunk 上空闲块数量(free list 是单向链表,这里按块地址判定)。
			size_t freeBlocks = 0;
			for (Node* node = m_FreeList; node; node = node->Next)
			{
				auto* address = reinterpret_cast<uint8_t*>(node);
				auto* base = static_cast<uint8_t*>(chunk->Data);
				if (address >= base && address < base + m_BlockSize * m_ObjectsPerChunk)
					++freeBlocks;
			}

			Chunk* next = chunk->Next;
			if (freeBlocks == m_ObjectsPerChunk && (previous != nullptr || next != nullptr))
			{
				// 该 chunk 全空且不是唯一 chunk:从 free list 摘除其块并释放。
				auto* base = static_cast<uint8_t*>(chunk->Data);
				Node** link = &m_FreeList;
				while (*link)
				{
					auto* address = reinterpret_cast<uint8_t*>(*link);
					if (address >= base && address < base + m_BlockSize * m_ObjectsPerChunk)
						*link = (*link)->Next;
					else
						link = &(*link)->Next;
				}
				if (previous)
					previous->Next = next;
				else
					m_ChunkList = next;
				m_Size -= m_ObjectSize * m_ObjectsPerChunk;
				_aligned_free(chunk->Data);
				delete chunk;
				chunk = next;
				++released;
				continue;
			}
			previous = chunk;
			chunk = next;
		}
		return released;
	}

	void PoolAllocator::Grow()
	{
		size_t chunkMemorySize = m_BlockSize * m_ObjectsPerChunk;
		void* raw = _aligned_malloc(chunkMemorySize, m_Alignment);

		// 统计总共从系统申请的内存（包含未使用的 Slots）
		m_Size += chunkMemorySize;

		// 将新申请的内存划分成若干 Node 链表
		for (size_t i = 0; i < m_ObjectsPerChunk; ++i)
		{
			Node* newNode = reinterpret_cast<Node*>(
				static_cast<uint8_t*>(raw) + (i * m_BlockSize)
				);
			newNode->Next = m_FreeList;
			m_FreeList = newNode;
		}

		// 记录 Chunk 以便析构
		Chunk* newChunk = new Chunk { raw, m_ChunkList };
		m_ChunkList = newChunk;
	}
}
