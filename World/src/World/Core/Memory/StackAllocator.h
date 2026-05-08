#pragma once
#include "Memory.h"
#include "MemoryTracker.h"
namespace World
{
	using Marker = size_t;

	/**
	 * @brief 魔法宏：创建一个自管理的栈空间
	 * 变量名为 name，它实际上是一个 ScopedStack 实例
	 */
	#define WLD_STACK_WIZARD(name, size) World::ScopedStack name(size,#name);

	 /**
	  * @brief 配合使用的分配宏
	  * 注意这里通过 name.GetAllocator() 拿到真正的分配器
	  */
	#define WLD_STACK_NEW(T, name, ...) name.GetAllocator().New<T>(__VA_ARGS__);



	  // 栈分配器（Stack Allocator）适用于临时对象的分配，支持快速分配和回滚到之前的标记位置
	  // Stack, 嵌套逻辑、递归、UI 布局, 作用域级, 支持 Marker 回滚，RAII 安全自动清理
	class StackAllocator : public Allocator
	{
	public:
		StackAllocator(size_t size, void* start, const char* debugName)
			: Allocator(size, start), m_CurrentPos(start), m_DebugName(debugName)
		{
			MemoryTracker::Get().Register(this, m_DebugName, AllocatorType::Stack);

			//WLD_CORE_INFO("Stack Allocator Created: Size = {} bytes, Start = {}", size, start);
		}

		~StackAllocator()
		{
			//WLD_CORE_INFO("Stack Allocator Destroyed: Start = {}", m_Start);

			MemoryTracker::Get().Unregister(this);
			Clear();
			m_Start = nullptr;
			m_CurrentPos = nullptr;
			m_DestructorChain = nullptr;
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

			//WLD_CORE_INFO("Stack Allocator FreeToMarker: Freed to marker {}, Current Used Memory = {} bytes", marker, m_UsedMemory);
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
		const char* m_DebugName;
	};

	// ScopedStack 是 StackAllocator 的一个封装，负责管理内存的申请和释放，确保 RAII 安全
	class ScopedStack
	{
	public:
		ScopedStack(size_t size, const char* debugName)
			: m_RawMemory(_aligned_malloc(size, 16)), // 1. 必须先申请物理内存
			m_Allocator(size, m_RawMemory, debugName)          // 2. 直接初始化成员变量
		{
			//WLD_CORE_INFO("ScopedStack Created: Size = {} bytes, Raw Memory = {}", size, m_RawMemory);
		}

		~ScopedStack()
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

		// 返回引用，减少拷贝和开销
		StackAllocator& GetAllocator() { return m_Allocator; }
		StackAllocator* operator->() { return &m_Allocator; }

		ScopedStack(const ScopedStack&) = delete;
		ScopedStack& operator=(const ScopedStack&) = delete;

	private:
		void* m_RawMemory = nullptr;
		StackAllocator m_Allocator;
	};
}