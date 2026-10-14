#pragma once

#include "AudioTypes.h"
#include "AudioBuffer.h"
#include <glm/glm.hpp>
#include <vector>
#include <array>
#include <string>
#include <memory>
#include <unordered_map>

namespace World::Audio
{
	// 1. Acoustic Rooms & Portals
	struct AcousticPortal
	{
		glm::vec3 position { 0.0f };
		glm::vec3 normal { 0.0f, 0.0f, 1.0f };
		float width { 2.0f };
		float height { 3.0f };
		bool isOpen { true };
	};

	WLD_API bool CalculatePortalDiffraction(
		const glm::vec3& listenerPos,
		const glm::vec3& emitterPos,
		const AcousticPortal& portal,
		glm::vec3& outVirtualEmitterPos,
		float& outDiffractionFactor,
		float& outLowPassCutoff);

	// 2. 5.1 / 7.1 VBAP Surround Panning
	struct SpeakerLayout8
	{
		float left { 0.7071f };
		float right { 0.7071f };
		float center { 0.0f };
		float lfe { 0.0f };
		float sideLeft { 0.0f };
		float sideRight { 0.0f };
		float rearLeft { 0.0f };
		float rearRight { 0.0f };
	};

	WLD_API void CalculateVbapSurround(
		const glm::vec3& emitterRelPos,
		SpeakerLayout8& outGains);

	// 3. Dynamic Early Reflections (Environment Size Perception)
	struct EarlyReflectionsResult
	{
		float delayMs { 20.0f };
		float gain { 0.25f };
		float estimatedRoomVolume { 120.0f }; // m^3
	};

	WLD_API EarlyReflectionsResult CalculateEarlyReflections(
		const glm::vec3& listenerPos,
		const std::array<float, 6>& wallDistances); // +X, -X, +Y, -Y, +Z, -Z

	// 4. Speed of Sound Delay
	inline float CalculateSpeedOfSoundDelaySeconds(float distanceMeters)
	{
		constexpr float kSpeedOfSound = 343.0f; // m/s in dry air at 20C
		return std::max(0.0f, distanceMeters / kSpeedOfSound);
	}

	// 5. Large-World Audio Clustering (Audio LOD)
	struct AudioEmitterClusterItem
	{
		uint64_t id { 0 };
		glm::vec3 position { 0.0f };
		float volume { 1.0f };
		std::string soundEvent;
	};

	struct ClusteredEmitterResult
	{
		glm::vec3 centroid { 0.0f };
		float combinedVolume { 1.0f };
		std::string soundEvent;
		uint32_t originalCount { 1 };
	};

	WLD_API std::vector<ClusteredEmitterResult> ClusterDistantEmitters(
		const std::vector<AudioEmitterClusterItem>& emitters,
		const glm::vec3& listenerPos,
		float clusterThresholdDistance = 25.0f,
		float clusterRadius = 12.0f);

	// 6. HDR Audio System
	class WLD_API HdrAudioSystem
	{
	public:
		HdrAudioSystem() = default;

		void SetWindowDb(float windowDb) noexcept { m_WindowDb = std::clamp(windowDb, 20.0f, 80.0f); }
		float GetWindowDb() const noexcept { return m_WindowDb; }

		void PostHdrEvent(float splDb) noexcept;
		void Update(float dtSeconds) noexcept;

		float GetCurrentCompressionGain() const noexcept;
		float GetTopSplDb() const noexcept { return m_TopSplDb; }

	private:
		float m_WindowDb { 40.0f };
		float m_TopSplDb { 60.0f };      // Baseline room ambient ~60 dB SPL
		float m_CurrentPeakSpl { 60.0f };
		float m_ReleaseSpeed { 12.0f };   // dB/sec decay
	};

	// 7. Next-Gen Facial Lip-Sync & Formant Extraction
	struct LipSyncFrame
	{
		float timeSeconds { 0.0f };
		float jawOpen { 0.0f };      // 0.0 -> 1.0
		float mouthPucker { 0.0f };  // 0.0 -> 1.0
		float mouthFunnel { 0.0f };  // 0.0 -> 1.0
		float mouthSmile { 0.0f };   // 0.0 -> 1.0
	};

	WLD_API std::vector<LipSyncFrame> GenerateLipSyncFrames(
		const AudioBuffer& buffer,
		float frameIntervalSeconds = 0.033f); // 30 fps

	WLD_API float CalculateHeadRadiationFactor(
		const glm::vec3& speakerPos,
		const glm::vec3& speakerForward,
		const glm::vec3& listenerPos,
		float& outLowPassCutoff);

	// 8. Noise Gate for Multiplayer Voice
	class WLD_API NoiseGate
	{
	public:
		NoiseGate() = default;

		void Configure(
			float thresholdDb = -42.0f,
			float attackMs = 5.0f,
			float releaseMs = 120.0f,
			float holdMs = 60.0f) noexcept;

		void ProcessStereo(float* left, float* right, uint32_t frameCount, float sampleRate) noexcept;
		bool IsGateOpen() const noexcept { return m_GateGain > 0.05f; }

	private:
		float m_ThresholdLinear { 0.008f }; // ~ -42 dB
		float m_AttackCoeff { 0.1f };
		float m_ReleaseCoeff { 0.005f };
		float m_HoldFrames { 2400.0f };
		float m_HoldCounter { 0.0f };
		float m_GateGain { 0.0f };
	};

	// 9. Procedural Weather Ambience
	class WLD_API ProceduralWeatherSystem
	{
	public:
		ProceduralWeatherSystem() = default;

		void Update(float windSpeedKmH, float rainIntensity01, float stormProbability01, float dtSeconds) noexcept;

		float GetWindGain() const noexcept { return m_WindGain; }
		float GetWindPitch() const noexcept { return m_WindPitch; }
		float GetRainGain() const noexcept { return m_RainGain; }
		bool CheckThunderTrigger() noexcept;

	private:
		float m_WindGain { 0.0f };
		float m_WindPitch { 1.0f };
		float m_RainGain { 0.0f };
		float m_ThunderCooldown { 15.0f };
		float m_ThunderTimer { 0.0f };
	};
}
