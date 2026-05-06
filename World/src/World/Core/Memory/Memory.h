#pragma once
#include <cstdint>
#include <cstddef>
#include <iostream>

namespace World
{
	#define WLD_NEW(T, allocator, ...) (allocator).New<T>(__VA_ARGS__)
	#define WLD_FRAME_NEW(T, ...) Application::Get().GetFrameAllocator().New<T>(__VA_ARGS__)

	// 析构回调：一个函数指针，知道如何把 void* 转回 T* 并调用 ~T()
	typedef void (*DestructorFunc)(void*);

	struct DestructorNode
	{
		DestructorFunc Callback = nullptr; // 这里的函数由编译器自动生成（模板魔法）
		void* Object = nullptr;            // 对象的地址
		DestructorNode* Next = nullptr;    // 指向下一个待办事项
	};

	struct MemoryPage
	{
		void* Data = nullptr;         // 实际数据区
		size_t Size = 0;        // 该页大小
		size_t Offset = 0;      // 当前页已分配到的偏移量,即下一个可用地址 = Data + Offset
		MemoryPage* Next = nullptr;   // 链表指针
	};

	/**
	 * @brief 内存对齐辅助函数
	 * @param address 需要对齐的地址
	 * @param alignment 对齐的字节数,必须是2的幂
	*/
	inline uintptr_t AlignForward(uintptr_t address, size_t alignment)
	{
		return (address + (static_cast<uintptr_t>(alignment) - 1)) & ~(static_cast<uintptr_t>(alignment) - 1);
	}

	class Allocator
	{
	public:
		/**
		 * @brief 工业级对象分配函数
		 * @tparam T 对象类型
		 * @tparam Args 构造函数参数类型
		 * @param args 传递给构造函数的参数
		 */
		template <typename T, typename... Args>
		T* New(Args&&... args)
		{
			void* raw = this->Allocate(sizeof(T), alignof(T));
			if (!raw) return nullptr;

			T* obj = new (raw) T(std::forward<Args>(args)...);

			// 工业级：如果对象有析构函数，自动注册
			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				// 如果子类没重写，这里就是空操作
				RegisterDestructor(obj, [](void* p) { static_cast<T*>(p)->~T(); });
			}

			return obj;
		}
		/**
		 * @brief 销毁对象（仅针对需要手动释放的对象，如 LOS 中的长寿对象）
		 */
		template <typename T>
		void Delete(T* p)
		{
			if (p)
			{
				p->~T();        // 手动调用析构函数
				this->Deallocate(p); // 归还内存（如果是 Linear，这里可能为空操作）
			}
		}
	public:
		Allocator(size_t size, void* start)
			: m_Size(size), m_Start(start), m_UsedMemory(0), m_NumAllocations(0)
		{}

		virtual ~Allocator() { m_Start = nullptr; m_Size = 0; }

		/**
		 * @brief 分配内存
		 * @param size 申请的大小
		 * @param alignment 对齐要求（通常是 4, 8, 16, 32）
		 */
		virtual void* Allocate(size_t size, size_t alignment = 8) = 0;

		/**
		 * @brief 释放内存
		 * @param p 指向待释放内存的指针
		 */
		virtual void Deallocate(void* p) {};

		void* GetStart() const { return m_Start; }
		size_t GetSize() const { return m_Size; }
		size_t GetUsedMemory() const { return m_UsedMemory; }
		size_t GetNumAllocations() const { return m_NumAllocations; }
	protected:
		virtual void RegisterDestructor(void* obj, DestructorFunc func)
		{
			// 默认实现：不注册任何析构回调，适用于不需要析构的分配器（如 Linear）
			// 需要析构的分配器（如 DualTrack）会重写这个方法，把回调挂到链表上
		};
	protected:
		void* m_Start = nullptr;           // 预分配大内存块的起始地址
		size_t m_Size = 0;           // 内存块的总容量 (Total Capacity)
		size_t m_UsedMemory = 0;     // 已使用的字节数
		size_t m_NumAllocations = 0; // 记录总分配次数，用于排查内存泄漏
	};
}