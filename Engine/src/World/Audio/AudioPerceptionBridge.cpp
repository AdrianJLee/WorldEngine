#include "AudioPerceptionBridge.h"
#include <algorithm>

namespace World::Audio
{
	void AudioPerceptionBridge::SetStimulusCallback(StimulusCallback callback)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Callback = std::move(callback);
	}

	void AudioPerceptionBridge::ClearStimulusCallback() noexcept
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Callback = nullptr;
	}

	void AudioPerceptionBridge::BroadcastStimulus(const SoundStimulus& stimulus)
	{
		StimulusCallback cb;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_TotalBroadcastCount++;
			m_RecentStimuli.push_back(stimulus);
			if (m_RecentStimuli.size() > m_MaxHistory)
			{
				m_RecentStimuli.erase(m_RecentStimuli.begin());
			}
			cb = m_Callback;
		}

		if (cb)
		{
			cb(stimulus);
		}
	}

	std::vector<SoundStimulus> AudioPerceptionBridge::GetRecentStimuli(size_t maxCount) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_RecentStimuli.empty() || maxCount == 0)
		{
			return {};
		}

		size_t count = std::min(maxCount, m_RecentStimuli.size());
		return std::vector<SoundStimulus>(m_RecentStimuli.end() - count, m_RecentStimuli.end());
	}

	void AudioPerceptionBridge::ClearHistory() noexcept
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_RecentStimuli.clear();
	}
}
