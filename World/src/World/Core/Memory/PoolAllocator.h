#pragma once
#include "Memory.h"
#include "MemoryTracker.h"
namespace World
{
	enum class PoolTier : size_t
	{
		Tiny = 64,    // 极少量的对象（如：全局配置、罕见状态）
		Small = 256,   // 少量（如：玩家、Boss、特殊特效）
		Medium = 1024,  // 中等（如：普通敌人、常规子弹）
		Large = 4096,  // 大量（如：粒子系统、大量碎片）
		Huge = 16384  // 极端情况
	};

	enum class PoolTag : size_t
	{
		// --- 核心维度 ---
		General = 0,    // 默认：通用对象

		// --- 系统维度 ---
		Rendering,      // 渲染系统专属（如：DrawCommands, MaterialInstances）
		Physics,        // 物理系统专属（如：CollisionManifolds, Constraints）
		ECS,            // 实体组件系统（如：各种 Component 存储）
		AI,             // 路径寻路、行为树节点

		// --- 场景维度 ---
		GameMode,       // 游戏运行时的对象
		Editor,         // 仅编辑器模式使用的对象（不希望混入游戏内存中）

		// --- 扩展维度 ---
		Tools,          // 临时分析工具、Debug 绘图对象
		Internal        // 引擎底层管理对象（如：DestructorNode 本身）
	};

	// 池分配器（Pool Allocator）适用于大量同类型小对象的分配，内部维护一个空闲链表来快速分配和回收内存块
	class PoolAllocator : public Allocator
	{
		// 每个 Node 代表一个可用的内存块，Next 指向下一个可用块
		struct Node
		{
			Node* Next = nullptr;
		};

		// 每个 Chunk 都包含一个大块内存和一个指向下一个 Chunk 的指针
		struct Chunk
		{
			void* Data = nullptr;
			Chunk* Next = nullptr;
		};

	public:
		PoolAllocator(const char* typeName, PoolTag tag, size_t objectSize, size_t objectAlignment, size_t objectsPerChunk = 64)
			: Allocator(0, nullptr), m_TypeName(typeName), m_Tag(tag), m_ObjectSize(std::max(objectSize, sizeof(Node))),
			m_Alignment(objectAlignment), m_ObjectsPerChunk(objectsPerChunk),
			m_FreeList(nullptr), m_ChunkList(nullptr)
		{
			MemoryTracker::Get().Register(this);
		}

		~PoolAllocator()
		{
			MemoryTracker::Get().Unregister(this);

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

		void* Allocate(size_t size, size_t alignment = 8) override
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

		void Deallocate(void* p) override
		{
			if (!p) return;

			// 将释放的内存块插回 Free List 头部
			Node* node = reinterpret_cast<Node*>(p);
			node->Next = m_FreeList;
			m_FreeList = node;

			m_UsedMemory -= m_ObjectSize;
			m_NumAllocations--;
		}

		PoolStats GetStats() const
		{
			return {
				m_TypeName,
				m_Tag,
				m_ObjectSize,
				m_UsedMemory,      // 已经在之前代码中实现
				m_TotalReserved,   // 需在 Grow() 时增加
				m_NumAllocations   // 已经在之前代码中实现
			};
		}
	private:
		// 申请新的 Chunk，并将其划分成 Node 链表
		void Grow()
		{
			size_t chunkMemorySize = m_ObjectSize * m_ObjectsPerChunk;
			void* raw = _aligned_malloc(chunkMemorySize, m_Alignment);

			// 统计总共从系统申请的内存（包含未使用的 Slots）
			m_TotalReserved += chunkMemorySize;

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

	private:
		size_t m_ObjectSize = 0; // 每个对象的大小（至少要能存下一个 Node）
		size_t m_Alignment = 0;
		size_t m_ObjectsPerChunk = 0; // 每个 Chunk 包含的对象数量


		Node* m_FreeList = nullptr;   // 指向第一个可用的空闲块
		Chunk* m_ChunkList = nullptr; // 指向所有分配的内存页，用于析构释放

		const char* m_TypeName;
		PoolTag m_Tag;
	};

	template<typename T, PoolTag Tag = PoolTag::General, PoolTier Tier = PoolTier::Medium>
	class PoolRegistry
	{
	public:
		static PoolAllocator& GetPool()
		{
			static const char* typeName = typeid(T).name();

			thread_local World::PoolAllocator s_Pool(
				typeName,
				Tag,
				sizeof(T),
				alignof(T),
				static_cast<size_t>(Tier)
			);
			return s_Pool;
		}
	};
	// 方便的宏定义，简化调用
	#define WLD_POOL_NEW_EX(T, Tag, Tier, ...) \
    World::PoolRegistry<T, Tag, Tier>::GetPool().New<T>(__VA_ARGS__)

	// 默认使用 General 标签和 Medium 级别的池
	#define WLD_POOL_NEW(T, ...) \
    WLD_POOL_NEW_EX(T, World::PoolTag::General, World::PoolTier::Medium, __VA_ARGS__)


	#define WLD_POOL_DELETE(T, Tag, ptr) World::PoolRegistry<T, Tag>::GetPool().Delete(ptr)
}