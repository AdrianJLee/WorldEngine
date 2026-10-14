#pragma once

#include "AudioTypes.h"
#include "AudioCommandQueue.h"
#include "AudioBuffer.h"
#include "VoicePool.h"
#include "MixerBus.h"
#include "SoundEvent.h"
#include "IAudioBackend.h"
#include "InteractiveMusic.h"
#include "AiAudioStudio.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace World
{
	class Scene;
}

namespace World::Audio
{
	struct ComboEscalator
	{
		int currentCombo = 0;
		float lastTriggerTime = 0.0f;
		float stepSemitones = 1.0f;
		float maxPitchRatio = 2.0f;
		float timeoutSeconds = 1.5f;
	};

	struct RapidFireTracker
	{
		float lastTriggerTime = -1.0f;
		uint32_t triggerCountInWindow = 0;
		VoiceHandle lastVoiceHandle = kInvalidVoiceHandle;
	};

	class WLD_API AudioSystem
	{
	public:
		static AudioSystem& Get();

		AudioSystem();
		~AudioSystem();

		bool Initialize(bool useNullBackend = false, uint32_t sampleRate = kDefaultSampleRate, uint32_t bufferFrames = kDefaultMixBlockFrames);
		void Shutdown();
		bool IsInitialized() const noexcept { return m_Initialized; }

		// Main thread frame update (syncs ECS transforms, music clock, commands)
		void Update(float deltaSeconds, Scene* activeScene = nullptr);

		// Event triggering & playback
		VoiceHandle PostEvent(
			const std::string& eventId,
			const glm::vec3& worldPos = glm::vec3(0.0f),
			float volumeScale = 1.0f,
			float pitchScale = 1.0f,
			bool looping = false,
			bool allowThrottling = true);

		VoiceHandle PostEvent2D(
			const std::string& eventId,
			float volumeScale = 1.0f,
			float pitchScale = 1.0f,
			bool looping = false,
			bool allowThrottling = true);

		VoiceHandle PostEvent2D(
			const std::string& eventId,
			const glm::vec2& screenPos,
			float volumeScale = 1.0f,
			float pitchScale = 1.0f,
			bool looping = false,
			bool allowThrottling = true);



		VoiceHandle PostComboEvent(
			const std::string& eventId,
			const std::string& comboGroup = "default",
			const glm::vec2& screenPos = glm::vec2(0.0f),
			float volumeScale = 1.0f);

		const ComboEscalator* GetComboEscalator(const std::string& comboGroup = "default") const;
		void ResetComboEscalator(const std::string& comboGroup = "default");
		void ConfigureComboEscalator(
			const std::string& comboGroup,
			float stepSemitones,
			float maxPitchRatio,
			float timeoutSeconds);

		// Rapid-fire throttling configuration
		void SetRapidFireThrottlingEnabled(bool enabled) noexcept { m_RapidFireThrottlingEnabled = enabled; }
		bool IsRapidFireThrottlingEnabled() const noexcept { return m_RapidFireThrottlingEnabled; }
		void ConfigureRapidFireThrottling(uint32_t threshold = 3, float windowSeconds = 0.010f) noexcept
		{
			m_RapidFireThreshold = threshold;
			m_RapidFireWindowSeconds = windowSeconds;
		}
		void SetPhaseJitterEnabled(bool enabled) noexcept { m_PhaseJitterEnabled = enabled; }
		bool IsPhaseJitterEnabled() const noexcept { return m_PhaseJitterEnabled; }
		float GetTotalTimeSeconds() const noexcept { return m_TotalTimeSeconds; }

		void StopVoice(VoiceHandle handle, float fadeSeconds = 0.05f);
		void PauseVoice(VoiceHandle handle);
		void ResumeVoice(VoiceHandle handle);
		void SetVoiceLooping(VoiceHandle handle, bool loop);
		void StopAll(float fadeSeconds = 0.05f);

		// Lock-free command queue
		bool EnqueueCommand(const AudioCommand& cmd);
		AudioCommandQueue& GetCommandQueue() noexcept { return m_CommandQueue; }
		const AudioCommandQueue& GetCommandQueue() const noexcept { return m_CommandQueue; }

		// Bus & Snapshot controls
		void SetBusVolume(BusId busId, float volume);
		void SetBusMute(BusId busId, bool mute);
		void SetBusSolo(BusId busId, bool solo);
		void TriggerSnapshot(const std::string& snapshotName, float transitionSeconds = 0.3f);
		void ClearSnapshot(float transitionSeconds = 0.3f);

		// Subsystems access
		MixerGraph& GetMixerGraph() noexcept { return m_Mixer; }
		const MixerGraph& GetMixerGraph() const noexcept { return m_Mixer; }

		VoicePool& GetVoicePool() noexcept { return m_VoicePool; }
		const VoicePool& GetVoicePool() const noexcept { return m_VoicePool; }

		InteractiveMusicManager& GetMusic() noexcept { return m_Music; }
		AiAudioStudioManager& GetAiStudio() noexcept { return m_AiStudio; }

		IAudioBackend* GetBackend() noexcept { return m_Backend.get(); }

		// Event and Asset Registry
		void RegisterSoundEvent(const SoundEvent& soundEvent);
		const SoundEvent* FindSoundEvent(const std::string& eventId) const;
		std::vector<std::string> GetRegisteredSoundEventIds() const;
		void RegisterAudioBuffer(const std::string& path, std::shared_ptr<AudioBuffer> buffer);
		std::shared_ptr<AudioBuffer> FindAudioBuffer(const std::string& path) const;

		// ECS entity voice tracking inspection
		const std::unordered_map<uint32_t, VoiceHandle>& GetEntityVoiceMap() const noexcept { return m_EntityVoiceMap; }

	private:
		void AudioRenderCallback(float* outLeft, float* outRight, uint32_t frameCount);
		void ProcessCommandQueue();
		void ExecuteCommand(const AudioCommand& cmd);

		bool m_Initialized { false };
		uint32_t m_SampleRate { kDefaultSampleRate };
		uint32_t m_BufferFrames { kDefaultMixBlockFrames };

		std::unique_ptr<IAudioBackend> m_Backend;
		VoicePool m_VoicePool;
		MixerGraph m_Mixer;
		InteractiveMusicManager m_Music;
		AiAudioStudioManager m_AiStudio;

		AudioCommandQueue m_CommandQueue;

		std::unordered_map<std::string, SoundEvent> m_SoundEvents;
		mutable std::unordered_map<std::string, std::shared_ptr<AudioBuffer>> m_AudioBuffers;

		// ECS Voice tracking & Zone state
		std::unordered_map<uint32_t, VoiceHandle> m_EntityVoiceMap;
		uint32_t m_ActiveZoneSnapshotId { 0 };

		// 2D & Combo & Throttling state
		float m_TotalTimeSeconds { 0.0f };
		std::unordered_map<std::string, ComboEscalator> m_ComboEscalators;
		std::unordered_map<std::string, RapidFireTracker> m_RapidFireTrackers;
		bool m_RapidFireThrottlingEnabled { true };
		uint32_t m_RapidFireThreshold { 3 };
		float m_RapidFireWindowSeconds { 0.010f };
		bool m_PhaseJitterEnabled { true };

		// Listener state
		glm::vec3 m_ListenerPosition { 0.0f };
		glm::vec3 m_ListenerRight { 1.0f, 0.0f, 0.0f };
		glm::vec3 m_ListenerForward { 0.0f, 0.0f, -1.0f };

		// Scratch bus mix buffers (reused across blocks to guarantee 0 heap allocation)
		std::unordered_map<BusId, std::pair<std::vector<float>, std::vector<float>>> m_BusMixBuffers;
		mutable std::mutex m_RegistryMutex;
	};
}
