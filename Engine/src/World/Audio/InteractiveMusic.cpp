#include "InteractiveMusic.h"
#include <algorithm>

namespace World::Audio
{
	InteractiveMusicClock::InteractiveMusicClock()
	{
		Configure(120.0f, 4);
	}

	void InteractiveMusicClock::Configure(float bpm, uint32_t beatsPerBar)
	{
		m_Bpm = std::clamp(bpm, 20.0f, 300.0f);
		m_BeatsPerBar = std::max(1u, beatsPerBar);
		m_SecondsPerBeat = 60.0f / m_Bpm;
		Reset();
	}

	void InteractiveMusicClock::Reset() noexcept
	{
		m_AccumulatedSeconds = 0.0f;
		m_CurrentBar = 0;
		m_CurrentBeat = 0;
		m_BeatProgress = 0.0f;
		m_CrossedBeat = false;
		m_CrossedBar = false;
	}

	void InteractiveMusicClock::Update(float deltaSeconds)
	{
		m_CrossedBeat = false;
		m_CrossedBar = false;

		if (deltaSeconds <= 0.0f) return;

		float prevTime = m_AccumulatedSeconds;
		m_AccumulatedSeconds += deltaSeconds;

		uint32_t prevTotalBeats = static_cast<uint32_t>(prevTime / m_SecondsPerBeat);
		uint32_t newTotalBeats = static_cast<uint32_t>(m_AccumulatedSeconds / m_SecondsPerBeat);

		if (newTotalBeats > prevTotalBeats)
		{
			m_CrossedBeat = true;
			m_CurrentBeat = newTotalBeats % m_BeatsPerBar;
			uint32_t prevBar = m_CurrentBar;
			m_CurrentBar = newTotalBeats / m_BeatsPerBar;
			if (m_CurrentBar > prevBar)
			{
				m_CrossedBar = true;
			}
		}

		float beatTime = std::fmod(m_AccumulatedSeconds, m_SecondsPerBeat);
		m_BeatProgress = beatTime / m_SecondsPerBeat;
	}

	InteractiveMusicManager::InteractiveMusicManager() = default;

	void InteractiveMusicManager::RegisterTrack(const MusicTrackDesc& desc)
	{
		m_Tracks[desc.trackId] = desc;
	}

	void InteractiveMusicManager::Play(const std::string& trackId, float fadeSeconds)
	{
		auto it = m_Tracks.find(trackId);
		if (it != m_Tracks.end())
		{
			m_CurrentTrackId = trackId;
			m_Clock.Configure(it->second.bpm, it->second.beatsPerBar);
			m_HasPendingTransition = false;
		}
	}

	void InteractiveMusicManager::TransitionTo(const std::string& targetTrackId, TransitionSync sync, float fadeSeconds)
	{
		if (sync == TransitionSync::Immediate)
		{
			Play(targetTrackId, fadeSeconds);
			return;
		}

		m_PendingTrackId = targetTrackId;
		m_PendingSync = sync;
		m_PendingFadeSec = fadeSeconds;
		m_HasPendingTransition = true;
	}

	void InteractiveMusicManager::Stop(float fadeSeconds)
	{
		m_CurrentTrackId.clear();
		m_PendingTrackId.clear();
		m_HasPendingTransition = false;
		m_Clock.Reset();
	}

	void InteractiveMusicManager::Update(float deltaSeconds)
	{
		m_Clock.Update(deltaSeconds);

		if (m_HasPendingTransition)
		{
			bool canExecute = false;
			if (m_PendingSync == TransitionSync::NextBeat && m_Clock.DidCrossBeat())
			{
				canExecute = true;
			}
			else if (m_PendingSync == TransitionSync::NextBar && m_Clock.DidCrossBar())
			{
				canExecute = true;
			}

			if (canExecute)
			{
				Play(m_PendingTrackId, m_PendingFadeSec);
			}
		}
	}
}
