#include "InteractiveMusic.h"
#include "AudioSystem.h"
#include <cmath>
#include <algorithm>

namespace World::Audio
{
	InteractiveMusicClock::InteractiveMusicClock()
	{
		Configure(120.0f, 4);
	}

	void InteractiveMusicClock::Configure(float bpm, uint32_t beatsPerBar)
	{
		m_Bpm = std::clamp(bpm, 20.0f, 400.0f);
		m_BeatsPerBar = (beatsPerBar > 0) ? beatsPerBar : 4;
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

		float prevSec = m_AccumulatedSeconds;
		m_AccumulatedSeconds += deltaSeconds;

		uint64_t prevTotalBeats = static_cast<uint64_t>(prevSec / m_SecondsPerBeat);
		uint64_t newTotalBeats = static_cast<uint64_t>(m_AccumulatedSeconds / m_SecondsPerBeat);

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

		StopAllStems(fadeSeconds);
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
		m_Clock.Update(deltaSeconds);
	}

	double InteractiveMusicManager::GetCalibratedDspTime(uint32_t sampleRate) const noexcept
	{
		if (sampleRate == 0) return 0.0;
		double rawTime = static_cast<double>(m_Clock.GetDspClockFrames()) / static_cast<double>(sampleRate);
		return std::max(0.0, rawTime - (static_cast<double>(m_AudioOffsetMs) * 0.001));
	}

	void InteractiveMusicManager::RegisterStem(const std::string& stemName, const std::string& soundEvent, float initialVolume)
	{
		MusicStemDesc desc;
		desc.stemName = stemName;
		desc.soundEvent = soundEvent;
		desc.targetVolume = std::clamp(initialVolume, 0.0f, 4.0f);
		desc.currentVolume = desc.targetVolume;
		desc.voiceHandle = kInvalidVoiceHandle;
		m_Stems[stemName] = desc;
	}

	void InteractiveMusicManager::SetStemVolume(const std::string& stemName, float volume, float fadeSeconds)
	{
		auto it = m_Stems.find(stemName);
		if (it != m_Stems.end())
		{
			it->second.targetVolume = std::clamp(volume, 0.0f, 4.0f);
			if (it->second.voiceHandle != kInvalidVoiceHandle)
			{
				AudioSystem::Get().SetVoiceVolume(it->second.voiceHandle, it->second.targetVolume);
			}
		}
	}

	float InteractiveMusicManager::GetStemVolume(const std::string& stemName) const
	{
		auto it = m_Stems.find(stemName);
		if (it != m_Stems.end())
		{
			return it->second.targetVolume;
		}
		return 0.0f;
	}

	void InteractiveMusicManager::StartAllStems(float fadeSeconds)
	{
		for (auto& [name, stem] : m_Stems)
		{
			if (stem.voiceHandle != kInvalidVoiceHandle)
			{
				AudioSystem::Get().StopVoice(stem.voiceHandle, 0.01f);
			}
			stem.voiceHandle = AudioSystem::Get().PostEvent2D(stem.soundEvent, stem.targetVolume, 1.0f, true);
		}
	}

	void InteractiveMusicManager::StopAllStems(float fadeSeconds)
	{
		for (auto& [name, stem] : m_Stems)
		{
			if (stem.voiceHandle != kInvalidVoiceHandle)
			{
				AudioSystem::Get().StopVoice(stem.voiceHandle, fadeSeconds);
				stem.voiceHandle = kInvalidVoiceHandle;
			}
		}
	}

	VoiceHandle InteractiveMusicManager::PlayStinger(const std::string& stingerSoundEvent, float volume)
	{
		if (stingerSoundEvent.empty()) return kInvalidVoiceHandle;
		return AudioSystem::Get().PostEvent2D(stingerSoundEvent, volume, 1.0f, false, false);
	}
}
