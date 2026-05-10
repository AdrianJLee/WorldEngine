#pragma once


namespace World
{
	struct JobDecl;

	class JobQueue
	{
	public:
		JobQueue();
		~JobQueue();

		void Push(JobDecl job);

		bool Pop(JobDecl& outJob);

		bool Steal(JobDecl& outJob);
	public:
		// 注意：为了简化实现，这里使用固定大小的环形缓冲区，实际应用中可能需要动态扩展
		static const uint32_t CAPACITY = 4096; // 必须是 2 的幂
	private:
		JobDecl* m_Jobs;

		// 顶部索引，消费者线程修改，生产者线程读取
		std::atomic<int64_t> m_Top { 0 }; //Steal从顶部窃取任务

		// 底部索引，生产者线程修改，消费者线程读取
		std::atomic<int64_t> m_Bottom { 0 }; //Pop从底部弹出任务
	};
}