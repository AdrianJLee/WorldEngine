#pragma once

namespace World
{
	// 任务函数类型定义，接受一个 void* 参数，返回 void
	using JobFunction = void(*)(void* data);

	// JobDeclaration,任务声明结构体，包含任务函数指针和任务数据指针
	struct JobDecl
	{
		JobFunction Entry; // 任务函数指针
		void* Data;       // 任务数据指针
	};

	// 任务计数器，用于跟踪正在执行的任务数量，支持原子操作以确保线程安全
	struct JobCounter
	{
		std::atomic<int> Count { 0 }; // 任务计数器，初始值为0
	};

}