#pragma once
#include "Memory.h"

namespace World
{
	using Marker = size_t;


	#define WLD_STACK_NEW(stack,T,...) (stack).New<T>(__VA_ARGS__)
	#define WLD_STACK_SCOPE(stack) World::StackScope stack##_scope(stack)

	/**
	* @brief 在当前作用域创建一个局部栈分配器
	* @param name 分配器变量名
	* @param size 栈的总字节大小
	*/
	#define WLD_CREATE_STACK_LOCAL(name, size) \
    void* name##_raw = _aligned_malloc(size, 16); \
    World::StackAllocator name(size, name##_raw)

	/**
	* @brief 销毁局部栈并归还原始内存给系统
	*/
	#define WLD_DESTROY_STACK_LOCAL(name) \
    _aligned_free(name##_raw)

	/**
	* @brief 创建一个指向堆上栈分配器的指针
	* @param size 栈的总字节大小
	*/
	#define WLD_CREATE_STACK_PTR(size) \
    new World::StackAllocator(size, _aligned_malloc(size, 16))

	/**
	* @brief 销毁栈指针及其管理的原始内存
	*/
	#define WLD_DESTROY_STACK_PTR(stackPtr) \
    if (stackPtr) { \
        void* raw = (stackPtr)->GetStart(); \
        delete (stackPtr); \
        _aligned_free(raw); \
    }

	// 栈分配器（Stack Allocator）适用于临时对象的分配，支持快速分配和回滚到之前的标记位置
	class StackAllocator : public Allocator
	{
	public:
		StackAllocator(size_t size, void* start)
			: Allocator(size, start), m_CurrentPos(start)
		{}

		~StackAllocator()
		{
			Clear();
		}


		/**
		 * @brief 分配内存
		 */
		virtual void* Allocate(size_t size, size_t alignment = 8) override
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

		/**
		 * @brief 获取当前内存位置的标记
		 */
		Marker GetMarker() const
		{
			return (uintptr_t)m_CurrentPos - (uintptr_t)m_Start;
		}

		/**
		 * @brief 释放内存到指定的标记位置
		 * @param marker 之前记录的位置
		 */
		void FreeToMarker(Marker marker)
		{
			uintptr_t targetAddr = (uintptr_t)m_Start + marker;

			if (m_DestructorChain)
			{
				// 遍历并调用回滚区域内的析构函数
				while ((uintptr_t)m_DestructorChain->Object >= targetAddr)
				{
					m_DestructorChain->Callback(m_DestructorChain->Object); // 执行析构
					m_DestructorChain = m_DestructorChain->Next;
				}
			}
			m_CurrentPos = reinterpret_cast<void*>(targetAddr);
			m_UsedMemory = marker;
		}

		/**
		 * @brief 全部清空
		 */
		void Clear()
		{
			FreeToMarker(0);
			m_NumAllocations = 0;
		}
	protected:
		virtual void RegisterDestructor(void* obj, DestructorFunc func) override
		{
			// 在栈上直接分配一个节点空间
			void* nodeMem = this->Allocate(sizeof(DestructorNode), alignof(DestructorNode));
			DestructorNode* node = new (nodeMem) DestructorNode { func, obj, m_DestructorChain };
			m_DestructorChain = node;
		}

	private:
		void* m_CurrentPos = nullptr;
		DestructorNode* m_DestructorChain = nullptr;
	};


	/**
	 * @brief RAII 辅助类：自动管理栈回滚
	 * 在进入作用域时记录 Marker，离开时自动释放该作用域内申请的所有内存
	 */
	class StackScope
	{
	public:
		StackScope(StackAllocator& allocator)
			: m_Allocator(allocator), m_Marker(allocator.GetMarker())
		{}

		~StackScope()
		{
			m_Allocator.FreeToMarker(m_Marker);
		}

		// 阻止拷贝，确保唯一所有权
		StackScope(const StackScope&) = delete;
		StackScope& operator=(const StackScope&) = delete;

	private:
		StackAllocator& m_Allocator;
		Marker m_Marker;
	};
}