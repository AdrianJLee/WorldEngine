#include "wldpch.h"
#include "World/Script/ScriptFileWatch.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace World
{
	ScriptFileWatch::ScriptFileWatch(double debounceSeconds)
	{
		if (std::isfinite(debounceSeconds) && debounceSeconds > 0.0)
			m_DebounceSeconds = debounceSeconds;
	}

	void ScriptFileWatch::Watch(const std::string& logicalPath)
	{
		if (logicalPath.empty())
			return;
		Entry entry;
		entry.Stable = FingerprintScriptSource(logicalPath);
		entry.Pending = entry.Stable;
		m_Entries[logicalPath] = std::move(entry);
	}

	void ScriptFileWatch::Unwatch(const std::string& logicalPath)
	{
		m_Entries.erase(logicalPath);
	}

	void ScriptFileWatch::Clear()
	{
		m_Entries.clear();
	}

	bool ScriptFileWatch::IsWatched(const std::string& logicalPath) const
	{
		return m_Entries.find(logicalPath) != m_Entries.end();
	}

	std::vector<std::string> ScriptFileWatch::Poll(double deltaSeconds)
	{
		std::vector<std::string> changed;
		if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0)
			deltaSeconds = 0.0;

		for (auto& [path, entry] : m_Entries)
		{
			const ScriptSourceFingerprint current = FingerprintScriptSource(path);
			if (current == entry.Stable)
			{
				// 内容没变,或改了又改回已确认内容 → 撤销未决变化。
				entry.HasPending = false;
				entry.Elapsed = 0.0;
				continue;
			}
			if (!entry.HasPending || current != entry.Pending)
			{
				// 观察到一份新的未决内容:重启 debounce 窗口(连续写归并成一次)。
				entry.Pending = current;
				entry.HasPending = true;
				entry.Elapsed = 0.0;
				continue;
			}
			entry.Elapsed += deltaSeconds;
			if (entry.Elapsed >= m_DebounceSeconds)
			{
				entry.Stable = entry.Pending;
				entry.HasPending = false;
				entry.Elapsed = 0.0;
				changed.push_back(path);
			}
		}

		std::sort(changed.begin(), changed.end());
		return changed;
	}

	std::vector<std::string> ScriptFileWatch::PollNow()
	{
		const auto now = std::chrono::steady_clock::now();
		double delta = 0.0;
		if (m_HasLastPoll)
			delta = std::chrono::duration<double>(now - m_LastPoll).count();
		m_LastPoll = now;
		m_HasLastPoll = true;
		return Poll(delta);
	}
}
