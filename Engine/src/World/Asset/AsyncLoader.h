#pragma once

#include "World/Core/Export.h"
#include "World/Core/StringPool.h"
#include "World/Core/Thread/Thread.h"
#include "World/Renderer/Mesh.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	// ---------------------------------------------------------------------------
	// T5c:异步资产加载器(两阶段,2026-10-04)。
	//
	// 加载一个 .wmodel 有三段,异步化的正确切法是**只搬 A+B**:
	//   A) 读盘(VFS/磁盘字节)              → 工作线程
	//   B) 解析 + Ref<Mesh> 构造(纯 CPU)     → 工作线程
	//   C) 上显存(CreateBuffer / RHI 句柄)   → **必须主线程**(见 §C 说明)
	//
	// C 段本来就已经是主线程的懒创建代码(Renderer3D 的 EnsureMeshBuffersFor,在绘制提交时),
	// 所以本类只负责把 A+B 搬走 —— GPU 路径一行都不用改。
	//
	// 契约:
	//   * 工作线程**禁止**触碰:`Mesh::WModelCache`(主线程提交专用)、RHI、ECS registry;
	//   * 结果先落在互斥保护的完成队列,由主线程在提交点 `PumpCompletions()` 入缓存;
	//   * 提交点是 `SceneRenderer::BeginScene`(**命令缓冲录制之前**)—— 与
	//     `AssetRegistry::BeginFrame` 同址,是每帧主线程的第一个架构点;
	//   * 确定性:走 `JobSystem::ParallelAllowed()` 门控;`WLD_NO_PARALLEL=1` 或 JobSystem
	//     未初始化时内联执行 ⇒ 异步与同步路径产出**逐字节相同**的 Mesh(共用
	//     `Mesh::BuildFromWModel`,一致性是结构性的而不是靠约定);
	//   * 生命周期:工作项持有本对象指针 ⇒ 析构前必须 Cancel + Wait(见 ~AsyncLoader)。
	//
	// 归属:`WorldContext::Resources()` 里的世界级服务(与 AssetRegistry 同层)。
	// ---------------------------------------------------------------------------
	class WLD_API AsyncLoader
	{
	public:
		enum class Phase : uint8_t
		{
			None = 0,   // 没请求过
			Pending,    // 工作线程正在做
			Ready,      // 工作线程做完,等主线程提交(或已提交)
			Failed,     // 读盘/解析失败(原因在 Error)
		};

		struct Status
		{
			Phase State = Phase::None;
			std::string Error;
		};

		AsyncLoader() = default;
		~AsyncLoader();
		AsyncLoader(const AsyncLoader&) = delete;
		AsyncLoader& operator=(const AsyncLoader&) = delete;

		// 请求异步加载(幂等):已就绪/在飞/已失败都直接返回现状,不重复排任务。
		// 若 JobSystem 未运行或并行被禁 ⇒ **内联同步执行**(结果与异步一致,便于 headless 测试)。
		//
		// startedNew 非空时写回"**本次调用**是否真的新起了请求"(幂等命中 ⇒ false)。
		// 它与返回的 Phase 是两件事:内联回退时一次调用就新起并立刻完成(Pending 从未被观察到),
		// 所以调用方统计"请求了多少资产"必须看这个出参,不能看 Phase。
		Status RequestMesh(PathId path, bool* startedNew = nullptr);

		// **主线程提交点**:把已完成项插进 Mesh 的进程内缓存,并清掉它们的记账。
		// 返回本次真正提交(首次入缓存)的条数。必须由主线程调用。
		std::size_t PumpCompletions();

		// 撤销某个路径的请求状态(资产被改动/淘汰时)。在飞的工作项无法中断,
		// 其完成结果会在 PumpCompletions 时被丢弃(不再入缓存)。
		void Cancel(PathId path);
		void Clear();

		Status Query(PathId path) const;

		std::size_t PendingCount() const;
		std::size_t ReadyCount() const;
		std::size_t FailedCount() const;
		// 加载界面用:"path" / "path (pending)" / "path (failed: why)" 的稳定排序清单。
		std::vector<std::string> DescribeEntries() const;
		// 单行摘要:"loaded=N pending=M failed=K"。
		std::string Describe() const;

	private:
		struct Entry
		{
			Phase State = Phase::None;
			std::string Error;
			Ref<Mesh> Prepared;   // 仅 Ready:等工作线程交接给主线程
		};

		// 工作线程侧:算出结果后登记(只碰 m_Mutex 保护的这两张表)。
		void Publish(PathId path, Ref<Mesh> mesh, std::string error);

		mutable std::mutex m_Mutex;
		std::unordered_map<PathId, Entry> m_Entries;
		std::vector<PathId> m_ReadyQueue;   // 稳定顺序(提交确定性)
		// 在飞工作项计数:析构/清理前必须等它归零,否则工作线程会写已释放对象。
		JobCounter m_InFlight;
	};
}
