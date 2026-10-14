#pragma once

#include "AudioTypes.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace World::Audio
{
	enum class TransitionSync : uint8_t
	{
		Immediate = 0,
		NextBeat = 1,
		NextBar = 2
	};

	struct MusicTrackDesc
	{
		std::string trackId;
		std::string soundEvent;
		float bpm { 120.0f };
		uint32_t beatsPerBar { 4 };
		float durationSeconds { 60.0f };
	};

	struct MusicStemDesc
	{
		std::string stemName;
		std::string soundEvent;
		float targetVolume { 1.0f };
		float currentVolume { 1.0f };
		VoiceHandle voiceHandle { kInvalidVoiceHandle };
	};

	class WLD_API InteractiveMusicClock
	{
	public:
		InteractiveMusicClock();

		void Configure(float bpm, uint32_t beatsPerBar = 4);
		void Reset() noexcept;

		void Update(float deltaSeconds);
		void AdvanceDspFrames(uint32_t frameCount, uint32_t sampleRate = kDefaultSampleRate);

		uint32_t GetCurrentBar() const noexcept { return m_CurrentBar; }
		uint32_t GetCurrentBeat() const noexcept { return m_CurrentBeat; }
		float GetBeatProgress() const noexcept { return m_BeatProgress; }
		uint64_t GetDspClockFrames() const noexcept { return m_DspClockFrames; }
		float GetAccumulatedSeconds() const noexcept { return m_AccumulatedSeconds; }

		bool DidCrossBeat() const noexcept { return m_CrossedBeat; }
		bool DidCrossBar() const noexcept { return m_CrossedBar; }

	private:
		float m_Bpm { 120.0f };
		uint32_t m_BeatsPerBar { 4 };
		float m_SecondsPerBeat { 0.5f };

		float m_AccumulatedSeconds { 0.0f };
		uint64_t m_DspClockFrames { 0 };
		uint32_t m_SampleRate { kDefaultSampleRate };
		uint32_t m_CurrentBar { 0 };
		uint32_t m_CurrentBeat { 0 };
		float m_BeatProgress { 0.0f };

		bool m_CrossedBeat { false };
		bool m_CrossedBar { false };
	};

	class WLD_API InteractiveMusicManager
	{
	public:
		InteractiveMusicManager();

		void RegisterTrack(const MusicTrackDesc& desc);

		void Play(const std::string& trackId, float fadeSeconds = 0.5f);
		void TransitionTo(const std::string& targetTrackId, TransitionSync sync = TransitionSync::NextBar, float fadeSeconds = 1.0f);
		void Stop(float fadeSeconds = 1.0f);

		void Update(float deltaSeconds);
		void AdvanceDspClock(uint32_t frameCount, uint32_t sampleRate = kDefaultSampleRate);

		const std::string& GetCurrentTrackId() const noexcept { return m_CurrentTrackId; }
		const InteractiveMusicClock& GetClock() const noexcept { return m_Clock; }
		VoiceHandle GetCurrentVoice() const noexcept { return m_CurrentVoice; }
		uint64_t GetDspClockFrames() const noexcept { return m_Clock.GetDspClockFrames(); }

		// Audio Latency Calibration
		void SetAudioOffsetMs(float offsetMs) noexcept { m_AudioOffsetMs = offsetMs; }
		float GetAudioOffsetMs() const noexcept { return m_AudioOffsetMs; }
		double GetCalibratedDspTime(uint32_t sampleRate = kDefaultSampleRate) const noexcept;

		// Vertical Remixing / Stems
		void RegisterStem(const std::string& stemName, const std::string& soundEvent, float initialVolume = 1.0f);
		void SetStemVolume(const std::string& stemName, float volume, float fadeSeconds = 0.5f);
		float GetStemVolume(const std::string& stemName) const;
		void StartAllStems(float fadeSeconds = 0.5f);
		void StopAllStems(float fadeSeconds = 0.5f);

		// Horizontal Stinger
		VoiceHandle PlayStinger(const std::string& stingerSoundEvent, float volume = 1.0f);

	private:
		std::unordered_map<std::string, MusicTrackDesc> m_Tracks;
		std::unordered_map<std::string, MusicStemDesc> m_Stems;
		InteractiveMusicClock m_Clock;

		VoiceHandle m_CurrentVoice { kInvalidVoiceHandle };
		std::string m_CurrentTrackId;
		std::string m_PendingTrackId;
		TransitionSync m_PendingSync { TransitionSync::NextBar };
		float m_PendingFadeSec { 1.0f };
		bool m_HasPendingTransition { false };

		float m_AudioOffsetMs { 0.0f };
	};
}
