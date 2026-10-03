#include "wldpch.h"
#include "World/Gameplay/SystemRegistry.h"

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

		uint32_t executed = 0;
		std::vector<Entry*> parallelEntries;
		std::vector<Entry*> serialEntries;
		for (Entry* entry : phaseEntries)
		{
			if (!entry->Update)
				continue;

			// 1. 条件门禁:返回 false 则本轮跳过
			if (entry->Desc.Condition && !entry->Desc.Condition())
				continue;

			// 2. 依赖检查
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

			// 3. 定时节流调度:如果定义了 Interval > 0
			if (entry->Desc.Interval > 0.0f)
			{
				entry->Accumulator += dt.GetSeconds();
				if (entry->Accumulator + 1e-4f < entry->Desc.Interval)
					continue;
			}

			(entry->Desc.ParallelSafe ? parallelEntries : serialEntries).push_back(entry);
		}

		const auto pushTiming = [this, phase](const Entry* entry, double milliseconds)
		{
			SystemTiming timing;
			timing.Name = entry->Desc.Name;
			timing.Phase = phase;
			timing.ParallelSafe = entry->Desc.ParallelSafe;
			timing.Milliseconds = milliseconds;
			m_Timings.push_back(std::move(timing));
		};

		// 并行安全系统:交给 JobSystem 并发执行
		if (!parallelEntries.empty())
		{
			std::vector<double> durations(parallelEntries.size(), 0.0);
			if (JobSystem::IsRunning() && parallelEntries.size() > 1)
			{
				struct Payload
				{
					Entry* Target;
					Timestep Dt;
					double* Out;
				};

				JobCounter counter;
				for (size_t i = 0; i < parallelEntries.size(); ++i)
				{
					Entry* entry = parallelEntries[i];
					const float stepDt = (entry->Desc.Interval > 0.0f) ? entry->Accumulator : dt.GetSeconds();
					JobDecl job;
					job.Emplace(Payload { entry, Timestep(stepDt), &durations[i] });
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

				for (size_t i = 0; i < parallelEntries.size(); ++i)
				{
					if (parallelEntries[i]->Desc.Interval > 0.0f)
						parallelEntries[i]->Accumulator = std::max(0.0f, parallelEntries[i]->Accumulator - parallelEntries[i]->Desc.Interval);
				}
			}
			else
			{
				for (size_t i = 0; i < parallelEntries.size(); ++i)
				{
					Entry* entry = parallelEntries[i];
					const float stepDt = (entry->Desc.Interval > 0.0f) ? entry->Accumulator : dt.GetSeconds();
					const auto begin = std::chrono::steady_clock::now();
					entry->Update(Timestep(stepDt));
					const auto end = std::chrono::steady_clock::now();
					durations[i] = std::chrono::duration<double, std::milli>(end - begin).count();
					if (entry->Desc.Interval > 0.0f)
						entry->Accumulator = std::max(0.0f, entry->Accumulator - entry->Desc.Interval);
				}
			}
			for (size_t i = 0; i < parallelEntries.size(); ++i)
			{
				pushTiming(parallelEntries[i], durations[i]);
				executed++;
			}
		}

		// 串行系统执行
		for (Entry* entry : serialEntries)
		{
			const float stepDt = (entry->Desc.Interval > 0.0f) ? entry->Accumulator : dt.GetSeconds();
			const auto begin = std::chrono::steady_clock::now();
			entry->Update(Timestep(stepDt));
			const auto end = std::chrono::steady_clock::now();
			if (entry->Desc.Interval > 0.0f)
				entry->Accumulator = std::max(0.0f, entry->Accumulator - entry->Desc.Interval);
			pushTiming(entry, std::chrono::duration<double, std::milli>(end - begin).count());
			executed++;
		}

		if (executed > 0)
			++m_RunCount;
		return executed;
	}
}
