#pragma once

#include "AudioTypes.h"
#include <glm/glm.hpp>
#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include <cstdint>

namespace World::Audio
{
	struct SoundStimulus
	{
		glm::vec3 position { 0.0f };
		float radius { 10.0f };
		float loudness { 1.0f };
		std::string tag { "noise" };
	};

	class WLD_API AudioPerceptionBridge
	{
	public:
		using StimulusCallback = std::function<void(const SoundStimulus&)>;

		AudioPerceptionBridge() = default;
		~AudioPerceptionBridge() = default;

		void SetStimulusCallback(StimulusCallback callback);
		void ClearStimulusCallback() noexcept;

		void BroadcastStimulus(const SoundStimulus& stimulus);

		uint64_t GetTotalStimuliBroadcast() const noexcept { return m_TotalBroadcastCount; }
		std::vector<SoundStimulus> GetRecentStimuli(size_t maxCount = 16) const;
		void ClearHistory() noexcept;

	private:
		StimulusCallback m_Callback;
		mutable std::mutex m_Mutex;
		uint64_t m_TotalBroadcastCount { 0 };
		std::vector<SoundStimulus> m_RecentStimuli;
		size_t m_MaxHistory { 64 };
	};
}
