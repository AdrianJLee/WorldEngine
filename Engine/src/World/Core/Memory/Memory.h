#pragma once

namespace World
{
	#pragma region Common

	// 析构回调：一个函数指针，知道如何把 void* 转回 T* 并调用 ~T()
	typedef void (*DestructorFunc)(void*);
	struct DestructorNode
	{
		DestructorFunc Callback = nullptr; // 这里的函数由编译器自动生成（模板魔法）
		void* Object = nullptr;            // 对象的地址
		DestructorNode* Next = nullptr;    // 指向下一个待办事项
	};

	#pragma endregion


	#pragma region Tracker

	// 增加分配器类型枚举，方便 UI 分类显示
	enum class AllocatorType
	{
		Unknown = 0,
		Linear,
		Stack,
		Pool,
		DualTrack
	};

	// 通用的分配器统计快照
	struct AllocatorStats
	{
		const char* Name;         // 分配器实例名称
		AllocatorType Type;       // 分配器类型
		size_t UsedBytes;         // 已使用字节
		size_t PeakBytes;         // 峰值已使用字节
		size_t TotalReserved;     // 总预留字节
		size_t NumAllocations;    // 活跃分配数
	};

	#pragma endregion

}
