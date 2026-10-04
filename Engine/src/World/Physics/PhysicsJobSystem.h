#pragma once

#include "World/Core/Export.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

#include <cstdint>

namespace World
{
	// Jolt 物理 ↔ 引擎 JobSystem 的适配器(JOBSYS 工业化)。
	//
	// 为什么继承 JPH::JobSystemWithBarrier:它是 Jolt 官方给"接入自研 job system"准备的基类,
	// Barrier 语义(等待/唤醒/执行 barrier 内任务)已经实现好;派生类只需要
	// GetMaxConcurrency / CreateJob / FreeJob / QueueJob / QueueJobs 五个钩子。
	//
	// **门控,默认不启用**:Jolt 多线程会改变约束求解的作业完成顺序,可能破坏 P4/P5/P6/P7
	// 已交付的"逐位可复现"承诺(同真实时间、不同帧率逐位相同)。只有项目清单
	// `physics.multithreaded: true` 且引擎 JobSystem 真的在跑时才走这里(见 Physics3D.cpp);
	// 默认仍是 JPH::JobSystemSingleThreaded,行为与今天逐字节相同。
	//
	// 引用计数口径与 Jolt 自己的 JobSystemThreadPool 完全一致(JobSystemThreadPool.cpp):
	//   * 入队即 AddRef(队列槽位 = 一个引用);
	//   * 队列消费者执行后**无条件** Release 一次 —— Job::Execute() 用 CAS 抢执行权,
	//     "抢不到"时返回非 done,但队列槽位的引用仍必须归还(否则 Job 对象泄漏)。
	class WLD_API PhysicsJobSystem final : public JPH::JobSystemWithBarrier
	{
	public:
		// maxBarriers 与 Jolt 自己的物理样例同量级(PhysicsSystem::Update 每次用一个 barrier)。
		explicit PhysicsJobSystem(uint32_t maxBarriers);
		~PhysicsJobSystem() override;

		// 引擎线程池在跑且有 worker 时才可用;否则调用方回退 JobSystemSingleThreaded。
		bool IsUsable() const;

	protected:
		int GetMaxConcurrency() const override;
		JPH::JobHandle CreateJob(const char* inName, JPH::ColorArg inColor, const JobFunction& inJobFunction,
			JPH::uint32 inNumDependencies) override;
		// Job / JobFunction 都是 JPH::JobSystem 的嵌套名字:在派生类作用域里用非限定名可见
		// (Job 还是 protected —— 写 JPH::Job 会撞访问控制)。
		void FreeJob(Job* inJob) override;
		void QueueJob(Job* inJob) override;
		void QueueJobs(Job** inJobs, JPH::uint inNumJobs) override;
	};
}
