#include "InteractiveMusic.h"
#include "AudioSystem.h"
#include <algorithm>
#include <cmath>

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
		m_DspClockFrames = 0;
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

	void InteractiveMusicClock::AdvanceDspFrames(uint32_t frameCount, uint32_t sampleRate)
	{
		m_CrossedBeat = false;
		m_CrossedBar = false;

		if (sampleRate == 0 || frameCount == 0) return;
		m_SampleRate = sampleRate;

		uint64_t prevFrames = m_DspClockFrames;
		m_DspClockFrames += frameCount;

		double prevSec = static_cast<double>(prevFrames) / static_cast<double>(m_SampleRate);
		double newSec = static_cast<double>(m_DspClockFrames) / static_cast<double>(m_SampleRate);
		m_AccumulatedSeconds = static_cast<float>(newSec);

		uint64_t prevTotalBeats = static_cast<uint64_t>(prevSec / static_cast<double>(m_SecondsPerBeat));
		uint64_t newTotalBeats = static_cast<uint64_t>(newSec / static_cast<double>(m_SecondsPerBeat));

		if (newTotalBeats > prevTotalBeats)
		{
			m_CrossedBeat = true;
			m_CurrentBeat = static_cast<uint32_t>(newTotalBeats % m_BeatsPerBar);
			uint32_t prevBar = m_CurrentBar;
			m_CurrentBar = static_cast<uint32_t>(newTotalBeats / m_BeatsPerBar);
			if (m_CurrentBar > prevBar)
			{
				m_CrossedBar = true;
			}
		}

		double beatTime = std::fmod(newSec, static_cast<double>(m_SecondsPerBeat));
		m_BeatProgress = static_cast<float>(beatTime / static_cast<double>(m_SecondsPerBeat));
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
			// Cross-fade: smooth transition from previous track
			if (m_CurrentVoice != kInvalidVoiceHandle)
			{
				AudioSystem::Get().StopVoice(m_CurrentVoice, fadeSeconds);
				m_CurrentVoice = kInvalidVoiceHandle;
			}

			m_CurrentTrackId = trackId;
			m_Clock.Configure(it->second.bpm, it->second.beatsPerBar);
			m_HasPendingTransition = false;

			// Trigger new track playback
			if (!it->second.soundEvent.empty())
			{
				m_CurrentVoice = AudioSystem::Get().PostEvent2D(it->second.soundEvent, 1.0f, 1.0f, true);
			}
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
		if (m_CurrentVoice != kInvalidVoiceHandle)
		{
			AudioSystem::Get().StopVoice(m_CurrentVoice, fadeSeconds);
			m_CurrentVoice = kInvalidVoiceHandle;
		}
		m_CurrentTrackId.clear();
		m_PendingTrackId.clear();
		m_HasPendingTransition = false;
		m_Clock.Reset();
	}

	void InteractiveMusicManager::AdvanceDspClock(uint32_t frameCount, uint32_t sampleRate)
	{
		m_Clock.AdvanceDspFrames(frameCount, sampleRate);

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

	void InteractiveMusicManager::Update(float deltaSeconds)
	{
		// Fallback to deltaSeconds clock only if DSP hardware clock has not started
		if (m_Clock.GetDspClockFrames() == 0)
		{
			m_Clock.Update(deltaSeconds);
		}

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
