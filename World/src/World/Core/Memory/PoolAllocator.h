#pragma once
#include "Allocator.h"

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
		Internal,       // 引擎底层管理对象（如：DestructorNode 本身）

		Script,         // 脚本系统专用（如：ScriptableEntity 实例）
	};

	#pragma region Macro

	// 方便的宏定义，简化调用
	#define WLD_POOL_NEW_EX(T, Tag, Tier, ...) \
    World::PoolRegistry<T, Tag, Tier>::GetPool().New<T>(__VA_ARGS__)

	// 默认使用 General 标签和 Medium 级别的池
	#define WLD_POOL_NEW(T, ...) \
    WLD_POOL_NEW_EX(T, World::PoolTag::General, World::PoolTier::Medium, __VA_ARGS__)

	#define WLD_POOL_DELETE(T, Tag, ptr) World::PoolRegistry<T, Tag>::GetPool().Delete(ptr)

	#pragma endregion


	// 池分配器（Pool Allocator）适用于大量同类型小对象的分配，内部维护一个空闲链表来快速分配和回收内存块
	// Pool, 大量同类小对象(Entity), 长期 / 不确定, 零碎片，O(1) 分配与随机销毁,频繁分配和销毁同类型对象的理想选择
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
		PoolAllocator(const char* debugName, PoolTag tag, size_t objectSize, size_t objectAlignment, size_t objectsPerChunk = 64);
		~PoolAllocator()override;

		void* Allocate(size_t size, size_t alignment = 8) override;

		void Deallocate(void* p) override;

	private:
		// 申请新的 Chunk，并将其划分成 Node 链表
		void Grow();
	private:
		size_t m_ObjectSize = 0; // 每个对象的大小（至少要能存下一个 Node）
		size_t m_Alignment = 0;
		size_t m_ObjectsPerChunk = 0; // 每个 Chunk 包含的对象数量


		Node* m_FreeList = nullptr;   // 指向第一个可用的空闲块
		Chunk* m_ChunkList = nullptr; // 指向所有分配的内存页，用于析构释放

		PoolTag m_Tag; // 池的标签（用于统计和调试）
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

}