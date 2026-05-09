#pragma once
#include "Allocator.h"

namespace World
{
	// 内存页结构：每个分配器都可以维护自己的内存页链表，方便管理和调试
	struct MemoryPage
	{
		void* Data = nullptr;         // 实际数据区
		size_t Size = 0;        // 该页大小
		size_t Offset = 0;      // 当前页已分配到的偏移量,即下一个可用地址 = Data + Offset
		MemoryPage* Next = nullptr;   // 链表指针
	};


	// 可增长线性分配器（Growable Linear Allocator）在 Linear Allocator 的基础上增加了自动增长功能，当当前页满了之后会申请新页继续分配，适用于不确定总内存需求但希望保持线性分配性能的场景
	// Growable, 无法预测大小的临时任务, 任务级, 基于 Page 链表，永不溢出
	class GrowableLinearAllocator : public Allocator
	{
	public:
		GrowableLinearAllocator(size_t pageSize, const char* debugName);

		~GrowableLinearAllocator() override;

		void* Allocate(size_t size, size_t alignment = 8) override;

		void Reset();

	private:
		MemoryPage* CreatePage(size_t size);

	private:
		size_t m_PageSize; // 每页的默认大小
		MemoryPage* m_HeadPage;// 指向链表头，Reset 时需要回到这里
		MemoryPage* m_CurrentPage;// 当前正在分配的页
	};
}