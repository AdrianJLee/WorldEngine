#pragma once


namespace World
{
	struct JobDecl;

	class JobQueue
	{
	public:
		JobQueue();
		~JobQueue();

		// 从顶部推入任务
		void Push(JobDecl& job);

		// 从底部弹出任务
		bool Pop(JobDecl& outJob);

		// 从顶部窃取任务
		bool Steal(JobDecl& outJob);
	public:

		static const uint32_t CAPACITY = 4096; // 必须是 2 的幂
	private:
		JobDecl* m_Jobs;

		// 顶部索引，消费者线程修改，生产者线程读取
		std::atomic<int64_t> m_Top { 0 }; //Steal从顶部窃取任务

		// 底部索引，生产者线程修改，消费者线程读取
		std::atomic<int64_t> m_Bottom { 0 }; //Pop从底部弹出任务
	};
}