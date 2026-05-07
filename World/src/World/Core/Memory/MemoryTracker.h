#pragma once

namespace World
{
	class PoolAllocator;
	enum class PoolTag : size_t;

	struct PoolStats
	{
		const char* TypeName; // 类型名称
		PoolTag Tag; // 分类标签
		size_t ObjectSize; // 每个对象的大小
		size_t UsedBytes; // 已使用的字节数（即正在被分配出去的内存总量）
		size_t TotalReserved; // 从系统申请的总物理内存（包含未使用的 Slots）
		size_t NumAllocations; // 当前活跃的分配数量（即正在被分配出去的对象数量）
	};

	class MemoryTracker
	{
	public:
		// 单例模式
		static MemoryTracker& Get()
		{
			static MemoryTracker instance;
			return instance;
		}

		// 注册：当一个新的 thread_local 池构造时调用
		void Register(PoolAllocator* allocator)
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Allocators.push_back(allocator);
		}

		// 注销：当线程退出，thread_local 池析构时调用
		void Unregister(PoolAllocator* allocator);

		// 获取快照：用于 ImGui 展示
		std::vector<PoolStats> GetSnapshot();

	private:
		std::vector<PoolAllocator*> m_Allocators;
		std::mutex m_Mutex;
	};
}