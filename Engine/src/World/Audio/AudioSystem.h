#pragma once

#include "AudioTypes.h"
#include "AudioRtpc.h"
#include "AudioCommandQueue.h"
#include "AudioBuffer.h"
#include "VoicePool.h"
#include "MixerBus.h"
#include "SoundEvent.h"
#include "IAudioBackend.h"
#include "InteractiveMusic.h"
#include "AiAudioStudio.h"
#include "AudioFft.h"
#include "AudioPerceptionBridge.h"
#include "AudioProfiler.h"
#include "AudioAcoustics.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <atomic>

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

	struct AudioZone2D
	{
		glm::vec2 minBounds { -10.0f, -10.0f };
		glm::vec2 maxBounds { 10.0f, 10.0f };
		float innerRadius { 5.0f };
		float outerBlendDistance { 3.0f };
		float lowPassCutoff { 20000.0f };
		float reverbSend { 0.0f };
		std::string snapshotName;
		float priority { 1.0f };
		bool isBox { true };
	};

	struct EventInstanceTracker
	{
		float lastTriggerTime { -100.0f };
		std::vector<VoiceHandle> activeVoices;
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

		VoiceHandle PostEvent(
			const std::string& eventId,
			const glm::vec3& worldPos,
			const std::unordered_map<std::string, std::string>& switchParams,
			const std::unordered_map<std::string, float>& blendParams,
			float volumeScale = 1.0f,
			float pitchScale = 1.0f,
			bool looping = false,
			bool allowThrottling = true);

		VoiceHandle PostEvent2D(
			const std::string& eventId,
			const glm::vec2& screenPos,
			const std::unordered_map<std::string, std::string>& switchParams,
			const std::unordered_map<std::string, float>& blendParams,
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

		// Pause Group and Time Dilation
		void SetPaused(bool paused) noexcept;
		bool IsPaused() const noexcept { return m_IsPaused; }
		void SetTimeDilation(float timeScale, float pitchScale = -1.0f) noexcept;
		float GetTimeDilation() const noexcept { return m_TimeDilation; }
		float GetTimeDilationPitch() const noexcept { return m_TimeDilationPitch; }

		// 2D Specialized Audio Extensions
		VoiceHandle PlayTileFootstep(int tileX, int tileY, const std::string& surfaceTag, const glm::vec2& worldPos);
		void AddAudioZone2D(const AudioZone2D& zone);
		void ClearAudioZones2D() noexcept { m_AudioZones2D.clear(); }
		void UpdateAudioZones2D(const glm::vec2& listenerPos);
		float CalculateBox2DOcclusion(const glm::vec2& listenerPos, const glm::vec2& emitterPos, bool hasWallBlocking) const noexcept;
		VoiceHandle TriggerSpriteFrameAudio(const std::string& soundEvent, const glm::vec2& spriteWorldPos, float volumeScale = 1.0f);

		// Next-Gen Acoustics Subsystems
		HdrAudioSystem& GetHdrSystem() noexcept { return m_HdrSystem; }
		const HdrAudioSystem& GetHdrSystem() const noexcept { return m_HdrSystem; }
		ProceduralWeatherSystem& GetWeatherSystem() noexcept { return m_WeatherSystem; }
		const ProceduralWeatherSystem& GetWeatherSystem() const noexcept { return m_WeatherSystem; }
		NoiseGate& GetVoiceNoiseGate() noexcept { return m_VoiceNoiseGate; }
		const NoiseGate& GetVoiceNoiseGate() const noexcept { return m_VoiceNoiseGate; }

		float CalculateSpeedOfSoundDelay(float distanceMeters) const noexcept { return CalculateSpeedOfSoundDelaySeconds(distanceMeters); }
		void CalculateVbapGains(const glm::vec3& relPos, SpeakerLayout8& outGains) const noexcept { CalculateVbapSurround(relPos, outGains); }

		// Volumetric, Velocity & Occlusion controls
		void SetVoiceVolume(VoiceHandle handle, float volume);
		void SetVoicePitch(VoiceHandle handle, float pitch);
		void SetVoiceVolumetricRadius(VoiceHandle handle, float radius);
		void SetVoiceVelocity(VoiceHandle handle, const glm::vec3& velocity);
		void SetVoiceOcclusion(VoiceHandle handle, float occlusion01);

		// Listener state getters & setters
		const glm::vec3& GetListenerPosition() const noexcept { return m_ListenerPosition; }
		void SetListenerPosition(const glm::vec3& pos) noexcept { m_ListenerPosition = pos; }
		const glm::vec3& GetListenerForward() const noexcept { return m_ListenerForward; }
		const glm::vec3& GetListenerRight() const noexcept { return m_ListenerRight; }
		const glm::vec3& GetListenerVelocity() const noexcept { return m_ListenerVelocity; }
		void SetListenerVelocity(const glm::vec3& vel) noexcept { m_ListenerVelocity = vel; }

		// Hardware DSP clock in seconds
		double GetDspTime() const noexcept;

		// Physics contact event integration
		void HandlePhysicsContact(
			const glm::vec3& contactPoint,
			float normalImpulse,
			const std::string& soundEvent = "footstep_player");

		void SetPhysicsContactThreshold(float threshold) noexcept { m_PhysicsContactThreshold = threshold; }
		float GetPhysicsContactThreshold() const noexcept { return m_PhysicsContactThreshold; }

		// Zone transition & reverb send state
		float GetCurrentZoneReverbSend() const noexcept { return m_CurrentZoneReverbSend; }
		float GetActiveZoneReverbSend() const noexcept { return m_ActiveZoneReverbSend; }
		void SetZoneTransitionRate(float rate) noexcept { m_ZoneTransitionRate = rate; }
		float GetZoneTransitionRate() const noexcept { return m_ZoneTransitionRate; }

		// Raycast occlusion callback & multi-ray evaluation
		void SetOcclusionRaycastCallback(std::function<float(const glm::vec3& listenerPos, const glm::vec3& sourcePos)> callback)
		{
			m_OcclusionRaycastCallback = std::move(callback);
		}
		void SetMultiRayOcclusionEnabled(bool enabled) noexcept { m_MultiRayOcclusionEnabled = enabled; }
		bool IsMultiRayOcclusionEnabled() const noexcept { return m_MultiRayOcclusionEnabled; }
		void SetOcclusionRaycastOffsets(float upOffset, float sideOffset) noexcept
		{
			m_OcclusionUpOffset = upOffset;
			m_OcclusionSideOffset = sideOffset;
		}
		float GetOcclusionUpOffset() const noexcept { return m_OcclusionUpOffset; }
		float GetOcclusionSideOffset() const noexcept { return m_OcclusionSideOffset; }

		float CalculateMultiRayOcclusion(const glm::vec3& listenerPos, const glm::vec3& sourcePos) const;

		// Bus & Snapshot controls
		void SetBusVolume(BusId busId, float volume);
		void SetBusMute(BusId busId, bool mute);
		void SetBusSolo(BusId busId, bool solo);
		void TriggerSnapshot(const std::string& snapshotName, float transitionSeconds = 0.3f);
		void ClearSnapshot(float transitionSeconds = 0.3f);

				// Rhythm & FFT Spectrum analysis
		void GetSpectrum(std::vector<float>& outBands, uint32_t bandCount = 64);
		bool IsBeatDetected() const noexcept;
		AudioFftAnalyzer& GetFftAnalyzer() noexcept { return m_FftAnalyzer; }
		const AudioFftAnalyzer& GetFftAnalyzer() const noexcept { return m_FftAnalyzer; }
		BeatDetector& GetBeatDetector() noexcept { return m_BeatDetector; }
		const BeatDetector& GetBeatDetector() const noexcept { return m_BeatDetector; }

		// AI Hearing / Sound Perception bridge
		AudioPerceptionBridge& GetPerceptionBridge() noexcept { return m_PerceptionBridge; }
		const AudioPerceptionBridge& GetPerceptionBridge() const noexcept { return m_PerceptionBridge; }
		void ConfigureEventPerception(const std::string& eventId, float radius, const std::string& tag = "noise");
		void BroadcastSoundStimulus(const SoundStimulus& stimulus);

		// Audio Profiler & Telemetry
		// RTPC Parameter controls
		AudioRtpcManager& GetRtpcManager() noexcept { return m_RtpcManager; }
		const AudioRtpcManager& GetRtpcManager() const noexcept { return m_RtpcManager; }
		void SetGlobalParameter(const std::string& name, float value);
		float GetGlobalParameter(const std::string& name) const;
		void SetVoiceParameter(VoiceHandle handle, const std::string& name, float value);
		float GetVoiceParameter(VoiceHandle handle, const std::string& name) const;

		// Footstep sound system with surface material adaptation
		VoiceHandle PlayFootstep(const std::string& surface, const glm::vec3& worldPos = glm::vec3(0.0f), float volume = 1.0f);

		AudioProfiler& GetProfiler() noexcept { return m_Profiler; }
		const AudioProfiler& GetProfiler() const noexcept { return m_Profiler; }

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
		AudioRtpcManager m_RtpcManager;
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
		glm::vec3 m_ListenerVelocity { 0.0f };
		glm::vec3 m_LastListenerPosition { 0.0f };
		bool m_HasLastListenerPos { false };

		// Zone reverb & smooth transition
		float m_ActiveZoneReverbSend { 0.0f };
		float m_CurrentZoneReverbSend { 0.0f };
		float m_ZoneTransitionRate { 3.0f };

		// Physics contact sound threshold
		float m_PhysicsContactThreshold { 0.5f };

		// Hardware DSP sample clock
		std::atomic<uint64_t> m_TotalRenderedFrames { 0 };

		// ECS transform tracking for velocities
		std::unordered_map<uint32_t, glm::vec3> m_EntityLastPosition;

		// Optional raycast occlusion callback and multi-ray parameters
		std::function<float(const glm::vec3& listenerPos, const glm::vec3& sourcePos)> m_OcclusionRaycastCallback;
		bool m_MultiRayOcclusionEnabled { true };
		float m_OcclusionUpOffset { 0.5f };
		float m_OcclusionSideOffset { 0.5f };

		// Scratch bus mix buffers (reused across blocks to guarantee 0 heap allocation)
		std::unordered_map<BusId, std::pair<std::vector<float>, std::vector<float>>> m_BusMixBuffers;
				// M4 Modules: FFT Analyzer, Beat Detector, Perception Bridge, Audio Profiler
		AudioFftAnalyzer m_FftAnalyzer;
		BeatDetector m_BeatDetector;
		AudioPerceptionBridge m_PerceptionBridge;
		AudioProfiler m_Profiler;
		std::unordered_map<std::string, SoundStimulusConfig> m_PerceptionConfigs;

		// Pause, Time Dilation & 2D Zones
		bool m_IsPaused { false };
		float m_TimeDilation { 1.0f };
		float m_TimeDilationPitch { 1.0f };
		std::vector<AudioZone2D> m_AudioZones2D;
		std::unordered_map<std::string, EventInstanceTracker> m_EventInstanceTrackers;

		// Next-Gen & Genre Subsystems
		HdrAudioSystem m_HdrSystem;
		ProceduralWeatherSystem m_WeatherSystem;
		NoiseGate m_VoiceNoiseGate;

		mutable std::mutex m_RegistryMutex;
	};
}



