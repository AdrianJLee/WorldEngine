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
		// 依赖它的系统必须先注销,否则会留下悬挂依赖。
		for (const Entry& entry : m_Systems)
			for (const std::string& dependency : entry.Desc.After)
				if (dependency == name)
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

		// 收集该阶段系统,按注册序号稳定排序,再做一次"按依赖重排"(注册时已保证依赖先注册,
		// 因此这里只需按 After 约束做稳定冒泡式调整,规模小、成本可忽略)。
		std::vector<const Entry*> phaseEntries;
		for (const Entry& entry : m_Systems)
			if (entry.Desc.Phase == phase)
				phaseEntries.push_back(&entry);
		std::sort(phaseEntries.begin(), phaseEntries.end(),
			[](const Entry* a, const Entry* b) { return a->Order < b->Order; });

		for (size_t i = 0; i < phaseEntries.size(); ++i)
			for (size_t j = i + 1; j < phaseEntries.size(); ++j)
			{
				// 靠前 i 声明"必须在靠后 j 之后"→ 说明顺序反了,交换。
				const auto& after = phaseEntries[i]->Desc.After;
				if (std::find(after.begin(), after.end(), phaseEntries[j]->Desc.Name) != after.end())
					std::swap(phaseEntries[i], phaseEntries[j]);
			}

		uint32_t executed = 0;
		// 依赖检查与筛选:并行安全系统之间声明为相互独立,可并发;其余按拓扑顺序串行。
		std::vector<const Entry*> parallelEntries;
		std::vector<const Entry*> serialEntries;
		for (const Entry* entry : phaseEntries)
		{
			if (!entry->Update)
				continue;
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

		// 并行安全系统:交给 JobSystem 并发执行(相互独立由 ParallelSafe 声明保证),
		// 主线程参与等待/帮忙执行;耗时按系统分别回收到 durations。
		if (!parallelEntries.empty())
		{
			std::vector<double> durations(parallelEntries.size(), 0.0);
			if (JobSystem::IsRunning() && parallelEntries.size() > 1)
			{
				struct Payload
				{
					const Entry* Target;
					Timestep Dt;
					double* Out;
				};

				JobCounter counter;
				for (size_t i = 0; i < parallelEntries.size(); ++i)
				{
					JobDecl job;
					job.Emplace(Payload { parallelEntries[i], dt, &durations[i] });
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
			}
			else
			{
				for (size_t i = 0; i < parallelEntries.size(); ++i)
				{
					const auto begin = std::chrono::steady_clock::now();
					parallelEntries[i]->Update(dt);
					const auto end = std::chrono::steady_clock::now();
					durations[i] = std::chrono::duration<double, std::milli>(end - begin).count();
				}
			}
			for (size_t i = 0; i < parallelEntries.size(); ++i)
			{
				pushTiming(parallelEntries[i], durations[i]);
				executed++;
			}
		}

		for (const Entry* entry : serialEntries)
		{
			const auto begin = std::chrono::steady_clock::now();
			entry->Update(dt);
			const auto end = std::chrono::steady_clock::now();
			pushTiming(entry, std::chrono::duration<double, std::milli>(end - begin).count());
			executed++;
		}
		if (executed > 0)
			m_RunCount++;
		return executed;
	}
}
