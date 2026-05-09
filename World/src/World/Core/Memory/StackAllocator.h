#pragma once
#include "Allocator.h"

namespace World
{
	using Marker = size_t;

	#pragma region Macro

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

	#pragma endregion


	// 栈分配器（Stack Allocator）适用于临时对象的分配，支持快速分配和回滚到之前的标记位置
	// Stack, 嵌套逻辑、递归、UI 布局, 作用域级, 支持 Marker 回滚，RAII 安全自动清理
	class StackAllocator : public Allocator
	{
	public:
		StackAllocator(size_t size, void* start, const char* debugName);


		~StackAllocator() override;

		virtual void* Allocate(size_t size, size_t alignment = 8) override;

		inline Marker GetMarker() const
		{
			return (uintptr_t)m_CurrentPos - (uintptr_t)m_Start;
		}

		/**
		 * @brief 释放内存到指定的标记位置
		 * @param marker 之前记录的位置
		 */
		void FreeToMarker(Marker marker);

		void Clear();

	protected:
		virtual void RegisterDestructor(void* obj, DestructorFunc func) override;

	private:
		void* m_CurrentPos = nullptr;
		DestructorNode* m_DestructorChain = nullptr;
	};

	// ScopedStack 是 StackAllocator 的一个封装，负责管理内存的申请和释放，确保 RAII 安全
	class ScopedStack
	{
	public:
		ScopedStack(size_t size, const char* debugName);
		~ScopedStack();

		ScopedStack(const ScopedStack&) = delete;
		ScopedStack& operator=(const ScopedStack&) = delete;

		StackAllocator& GetAllocator() { return m_Allocator; }
		StackAllocator* operator->() { return &m_Allocator; }

	private:
		void* m_RawMemory = nullptr;
		StackAllocator m_Allocator;
	};


}