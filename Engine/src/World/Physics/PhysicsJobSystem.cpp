#include "wldpch.h"
#include "World/Physics/PhysicsJobSystem.h"

#include "World/Core/Thread/JobSystem.h"

namespace World
{
	PhysicsJobSystem::PhysicsJobSystem(uint32_t maxBarriers)
		: JPH::JobSystemWithBarrier(maxBarriers)
	{
	}

	PhysicsJobSystem::~PhysicsJobSystem() = default;

	bool PhysicsJobSystem::IsUsable() const
	{
		// 引擎线程池没跑(或没有 worker)时,JobSystem::Kick 会退化成"提交线程内联执行",
		// 没有任何并行度;此时调用方必须回退 JobSystemSingleThreaded。
		return World::JobSystem::IsRunning() && World::JobSystem::WorkerCount() > 0;
	}

	int PhysicsJobSystem::GetMaxConcurrency() const
	{
		// 引擎池的 worker 数(派工单 §2.1 冻结口径;Jolt 自己的 ThreadPool 口径是 threads+1,
		// 这里更保守 —— 少估并发只会降低并行度,不会丢正确性)。
		return static_cast<int>(World::JobSystem::WorkerCount());
	}

	JPH::JobHandle PhysicsJobSystem::CreateJob(const char* inName, JPH::ColorArg inColor,
		const JobFunction& inJobFunction, JPH::uint32 inNumDependencies)
	{
		Job* job = new Job(inName, inColor, this, inJobFunction, inNumDependencies);

		// 与 JobSystemThreadPool::CreateJob 同口径:0 依赖 = 立即入队(CreateJob 的契约就是
		// "the job is started immediately if inNumDependencies == 0"),不能只等 barrier 的 Wait。
		JPH::JobHandle handle(job);
		if (inNumDependencies == 0)
			QueueJob(job);
		return handle;
	}

	void PhysicsJobSystem::FreeJob(Job* inJob)
	{
		delete inJob;
	}

	void PhysicsJobSystem::QueueJob(Job* inJob)
	{
		// 入队即持有引用(队列槽位算一个引用),与 JobSystemThreadPool::QueueJobInternal 一致。
		inJob->AddRef();

		World::JobDecl decl;
		decl.Priority = World::JobPriority::Normal;
		decl.Emplace(inJob);   // 负载 = Job*;Entry 里解一层指针取出它。
		decl.Entry = [](void* payload)
		{
			Job* job = *static_cast<Job**>(payload);
			job->Execute();
			// 无条件 Release(= JobSystemThreadPool::ThreadMain 的 worker 口径)。
			// Job::Execute() 被 barrier 的 Wait 线程抢走时会返回非 done,但"队列槽位"的
			// 这个引用必须归还:漏掉就永远不 FreeJob(Job 对象泄漏)。
			// 此时 Job 至少还被 barrier 持有的引用保活(JobSystemWithBarrier::AddJob 里
			// AddRef,Wait 里 IsDone 后才 Release),所以不会 use-after-free。
			job->Release();
		};
		World::JobSystem::Kick(std::move(decl));
	}

	void PhysicsJobSystem::QueueJobs(Job** inJobs, JPH::uint inNumJobs)
	{
		// 不自己造批量语义:逐个走 QueueJob(Kick 本来就是逐任务入队)。
		for (JPH::uint i = 0; i < inNumJobs; ++i)
			QueueJob(inJobs[i]);
	}
}
