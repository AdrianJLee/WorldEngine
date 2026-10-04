#include "wldpch.h"
#include "World/Gameplay/Runtime/SystemRegistry.h"

#include "World/Core/Log.h"
#include "World/Core/Thread/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace World::Gameplay
{
	namespace
	{
		constexpr size_t kPhaseCount = static_cast<size_t>(SystemPhase::Count);

		bool ContainsId(const std::vector<entt::id_type>& ids, entt::id_type id)
		{
			return std::find(ids.begin(), ids.end(), id) != ids.end();
		}

		// 两个系统能否进同一批次并行执行:
		// - 双方都已声明读写集:无冲突 ⟺ Write ∩ (Read ∪ Write) 为空(读-读不冲突);
		// - 任一方未声明:退化为 legacy 语义 —— 双方都显式 ParallelSafe 才可同批(保守串行)。
		bool ParallelCompatible(const SystemDesc& a, const SystemDesc& b)
		{
			// 并发派发的两个前提必须**同时**成立:
			//   1) 双方都声明"可以离线线程执行"(ParallelSafe)—— 这是**权限**;
			//   2) 读写集不冲突(声明过的才判)—— 这是**正确性**。
			// 只声明数据依赖不等于可以并行:漏写 ParallelSafe(默认 false)的系统必须保持串行 ——
			// 否则主线程专用资源(典型:Luau VM / 只在主线程持有的句柄)会被放到工作线程上执行。
			if (!a.ParallelSafe || !b.ParallelSafe)
				return false;
			if (!a.AccessDeclared || !b.AccessDeclared)
				return true;   // 双方都声明可并行,但没有读写集 ⇒ 沿用 legacy 的"信任"口径

			for (entt::id_type write : a.Writes)
				if (ContainsId(b.Writes, write) || ContainsId(b.Reads, write))
					return false;
			for (entt::id_type read : a.Reads)
				if (ContainsId(b.Writes, read))
					return false;
			return true;
		}
	}

	const char* SystemPhaseName(SystemPhase phase)
	{
		switch (phase)
		{
		case SystemPhase::PreFixed:  return "PreFixed";
		case SystemPhase::Fixed:     return "Fixed";
		case SystemPhase::Update:    return "Update";
		case SystemPhase::Late:      return "Late";
		case SystemPhase::PreRender: return "PreRender";
		default:                     return "Unknown";
		}
	}

	bool SystemRegistry::HasSystem(const std::string& name) const
	{
		return std::any_of(m_Systems.begin(), m_Systems.end(),
			[&name](const Entry& entry) { return entry.Desc.Name == name; });
	}

	bool SystemRegistry::SetEnabled(const std::string& name, bool enabled)
	{
		for (Entry& entry : m_Systems)
		{
			if (entry.Desc.Name != name)
				continue;
			entry.Desc.Enabled = enabled;
			return true;   // 名字存在即成功(重复设置幂等)
		}
		return false;
	}

	bool SystemRegistry::IsEnabled(const std::string& name) const
	{
		for (const Entry& entry : m_Systems)
			if (entry.Desc.Name == name)
				return entry.Desc.Enabled;
		return false;
	}

	bool SystemRegistry::SetOwner(const std::string& name, std::string owner)
	{
		for (Entry& entry : m_Systems)
		{
			if (entry.Desc.Name != name)
				continue;
			entry.Desc.Owner = std::move(owner);
			return true;
		}
		return false;
	}

	bool SystemRegistry::Register(const SystemDesc& desc, UpdateFn update)
	{
		m_LastError.clear();
		if (desc.Name.empty() || !update)
		{
			m_LastError = "system requires a name and an update function";
			return false;
		}
		if (static_cast<size_t>(desc.Phase) >= kPhaseCount)
		{
			m_LastError = "invalid phase for system '" + desc.Name + "'";
			return false;
		}
		if (HasSystem(desc.Name))
		{
			m_LastError = "system '" + desc.Name + "' is already registered";
			return false;
		}
		// 依赖允许"前向引用"(后注册的系统),因此这里只拦自依赖;
		// 依赖是否真的存在、是否成环,在 RunPhase 时按阶段判定(缺失则跳过该系统并记录错误)。
		if (std::find(desc.After.begin(), desc.After.end(), desc.Name) != desc.After.end())
		{
			m_LastError = "system '" + desc.Name + "' cannot depend on itself";
			return false;
		}

		Entry entry;
		entry.Desc = desc;
		entry.Update = std::move(update);
		entry.Order = m_NextOrder++;
		m_Systems.push_back(std::move(entry));
		return true;
	}

	bool SystemRegistry::Unregister(const std::string& name)
	{
		const auto it = std::find_if(m_Systems.begin(), m_Systems.end(),
			[&name](const Entry& entry) { return entry.Desc.Name == name; });
		if (it == m_Systems.end())
			return false;
		// 正在被其它系统依赖的系统不允许注销,防悬垂依赖。
		for (const Entry& entry : m_Systems)
			for (const std::string& after : entry.Desc.After)
				if (after == name)
				{
					m_LastError = "system '" + entry.Desc.Name + "' still depends on '" + name + "'";
					return false;
				}
		m_Systems.erase(it);
		return true;
	}

	void SystemRegistry::Clear()
	{
		m_Systems.clear();
		m_Timings.clear();
		m_NextOrder = 0;
		m_LastError.clear();
	}

	uint32_t SystemRegistry::RunPhase(SystemPhase phase, Timestep dt)
	{
		m_Timings.clear();
		m_LastError.clear();

		std::vector<Entry*> phaseEntries;
		for (Entry& entry : m_Systems)
			if (entry.Desc.Phase == phase)
				phaseEntries.push_back(&entry);
		std::sort(phaseEntries.begin(), phaseEntries.end(),
			[](const Entry* a, const Entry* b) { return a->Order < b->Order; });

		for (size_t i = 0; i < phaseEntries.size(); ++i)
			for (size_t j = i + 1; j < phaseEntries.size(); ++j)
			{
				const auto& after = phaseEntries[i]->Desc.After;
				if (std::find(after.begin(), after.end(), phaseEntries[j]->Desc.Name) != after.end())
					std::swap(phaseEntries[i], phaseEntries[j]);
			}

		// 1. 按拓扑序生成执行计划:批次(批内可并行、批间严格保序)+ 耗时顺序表。
		//    Enabled=false:保留在册,写 timing(0 ms),不执行、不进批;
		//    Condition=false / 依赖缺失 / Interval 未到:跳过执行且不占批次。
		std::vector<std::vector<Entry*>> batches;
		std::vector<Entry*> timingOrder;
		for (Entry* entry : phaseEntries)
		{
			if (!entry->Desc.Enabled)
			{
				timingOrder.push_back(entry);
				continue;
			}
			if (!entry->Update)
				continue;

			// 条件门禁:返回 false 则本轮跳过
			if (entry->Desc.Condition && !entry->Desc.Condition())
				continue;

			// 依赖检查
			bool dependenciesSatisfied = true;
			for (const std::string& dependency : entry->Desc.After)
			{
				if (!HasSystem(dependency))
				{
					m_LastError = "system '" + entry->Desc.Name + "' skipped: missing dependency '" + dependency + "'";
					WLD_CORE_WARN("[systems] {0}", m_LastError);
					dependenciesSatisfied = false;
					break;
				}
			}
			if (!dependenciesSatisfied)
				continue;

			// 定时节流调度:如果定义了 Interval > 0
			if (entry->Desc.Interval > 0.0f)
			{
				entry->Accumulator += dt.GetSeconds();
				if (entry->Accumulator + 1e-4f < entry->Desc.Interval)
					continue;
			}

			// 相位内分批:与当前批次内所有系统都无冲突才能同批,否则另起一批。
			if (batches.empty())
			{
				batches.emplace_back();
			}
			else
			{
				const auto& current = batches.back();
				const bool compatible = std::all_of(current.begin(), current.end(),
					[entry](const Entry* member) { return ParallelCompatible(member->Desc, entry->Desc); });
				if (!compatible)
					batches.emplace_back();
			}
			batches.back().push_back(entry);
			timingOrder.push_back(entry);
		}

		const auto pushTiming = [this, phase](const Entry* entry, double milliseconds)
		{
			SystemTiming timing;
			timing.Name = entry->Desc.Name;
			timing.Phase = phase;
			timing.ParallelSafe = entry->Desc.ParallelSafe;
			timing.Milliseconds = milliseconds;
			timing.Owner = entry->Desc.Owner;
			timing.Enabled = entry->Desc.Enabled;
			m_Timings.push_back(std::move(timing));
		};

		// 2. 批间严格保序:前一批全部结束才起下一批,保证 After 与隐式写依赖不被破坏。
		//    批内 size >= 2 且 JobSystem 运行时走 Kick/Wait 并发,否则串行执行该批次。
		std::unordered_map<Entry*, double> durations;
		uint32_t executed = 0;
		for (std::vector<Entry*>& batch : batches)
		{
			if (batch.size() >= 2 && JobSystem::IsRunning())
			{
				struct Payload
				{
					Entry* Target;
					Timestep Dt;
					double* Out;
				};

				std::vector<double> batchDurations(batch.size(), 0.0);
				JobCounter counter;
				for (size_t i = 0; i < batch.size(); ++i)
				{
					Entry* entry = batch[i];
					const float stepDt = (entry->Desc.Interval > 0.0f) ? entry->Accumulator : dt.GetSeconds();
					JobDecl job;
					job.Emplace(Payload { entry, Timestep(stepDt), &batchDurations[i] });
					job.Entry = [](void* data)
					{
						auto* payload = static_cast<Payload*>(data);
						const auto begin = std::chrono::steady_clock::now();
						payload->Target->Update(payload->Dt);
						const auto end = std::chrono::steady_clock::now();
						*payload->Out = std::chrono::duration<double, std::milli>(end - begin).count();
					};
					job.Counter = &counter;
					JobSystem::Kick(std::move(job));
				}
				JobSystem::Wait(&counter);

				for (size_t i = 0; i < batch.size(); ++i)
				{
					Entry* entry = batch[i];
					if (entry->Desc.Interval > 0.0f)
						entry->Accumulator = std::max(0.0f, entry->Accumulator - entry->Desc.Interval);
					durations[entry] = batchDurations[i];
					executed++;
				}
			}
			else
			{
				for (Entry* entry : batch)
				{
					const float stepDt = (entry->Desc.Interval > 0.0f) ? entry->Accumulator : dt.GetSeconds();
					const auto begin = std::chrono::steady_clock::now();
					entry->Update(Timestep(stepDt));
					const auto end = std::chrono::steady_clock::now();
					if (entry->Desc.Interval > 0.0f)
						entry->Accumulator = std::max(0.0f, entry->Accumulator - entry->Desc.Interval);
					durations[entry] = std::chrono::duration<double, std::milli>(end - begin).count();
					executed++;
				}
			}
		}

		// 3. 耗时表始终按注册/拓扑序输出(面板依赖次序);未执行者 0 ms。
		for (Entry* entry : timingOrder)
		{
			const auto it = durations.find(entry);
			pushTiming(entry, it == durations.end() ? 0.0 : it->second);
		}

		if (executed > 0)
			++m_RunCount;
		return executed;
	}
}
