#pragma once

#include "AudioTypes.h"
#include "AudioBuffer.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace World::Audio
{
	enum class FilterType : uint8_t
	{
		LowPass = 0,
		HighPass = 1,
		BandPass = 2,
		PeakingEQ = 3,
		LowShelf = 4,
		HighShelf = 5
	};

	class WLD_API BiquadFilter
	{
	public:
		BiquadFilter() = default;

		void Configure(FilterType type, float frequencyHz, float sampleRate, float q = 0.7071f, float gainDb = 0.0f);
		void Reset() noexcept;

		float Process(float inSample) noexcept;

	private:
		float m_B0 { 1.0f }, m_B1 { 0.0f }, m_B2 { 0.0f };
		float m_A1 { 0.0f }, m_A2 { 0.0f };
		float m_X1 { 0.0f }, m_X2 { 0.0f };
		float m_Y1 { 0.0f }, m_Y2 { 0.0f };
	};

	class WLD_API DynamicsCompressor
	{
	public:
		DynamicsCompressor() = default;

		void Configure(float thresholdDb, float ratio, float attackMs, float releaseMs, float sampleRate);
		void Reset() noexcept;

		void ProcessStereo(float& left, float& right) noexcept;

	private:
		float m_ThresholdLinear { 1.0f };
		float m_Ratio { 4.0f };
		float m_AttackCoeff { 0.9f };
		float m_ReleaseCoeff { 0.999f };
		float m_Envelope { 0.0f };
	};

	class WLD_API TruePeakLimiter
	{
	public:
		TruePeakLimiter() = default;

		void Configure(float ceilingDb = -0.1f, float releaseMs = 50.0f, float sampleRate = static_cast<float>(kDefaultSampleRate));
		void Reset() noexcept;

		void ProcessStereo(float& left, float& right) noexcept;

	private:
		float m_CeilingLinear { 0.9885f };
		float m_ReleaseCoeff { 0.999f };
		float m_Gain { 1.0f };
	};

	class WLD_API Bitcrusher
	{
	public:
		Bitcrusher() = default;

		void Configure(int bitDepth = 8, int downsampleFactor = 4) noexcept;
		void Reset() noexcept;

		void Process(float* inOutLeft, float* inOutRight, uint32_t frameCount) noexcept;
		void ProcessSample(float& inOutLeft, float& inOutRight) noexcept;

		int GetBitDepth() const noexcept { return m_BitDepth; }
		int GetDownsampleFactor() const noexcept { return m_DownsampleFactor; }

	private:
		int m_BitDepth { 8 };
		int m_DownsampleFactor { 4 };
		int m_DownsampleCounter { 0 };
		float m_HeldLeft { 0.0f };
		float m_HeldRight { 0.0f };
	};

	struct BinauralCues
	{
		float leftGain { 0.7071f };
		float rightGain { 0.7071f };
		float rearFilterCutoff { 20000.0f }; // Pinna shadowing cutoff for rear sounds
		float rearGain { 1.0f };             // Rear attenuation factor (3~5dB down)
		float elevationFactor { 0.0f };      // Height cue: -1.0 (below) to +1.0 (above)
	};

	class WLD_API SpatialCalculator
	{
	public:
		static float CalculateDistanceAttenuation(
			float distance,
			float minDistance,
			float maxDistance,
			AttenuationModel model) noexcept;

		static float CalculateConeAttenuation(
			const glm::vec3& sourcePos,
			const glm::vec3& sourceForward,
			const glm::vec3& listenerPos,
			float innerAngleDeg,
			float outerAngleDeg,
			float outerGain) noexcept;

		static void CalculateStereoPan(
			const glm::vec3& sourcePos,
			const glm::vec3& listenerPos,
			const glm::vec3& listenerRight,
			float& outLeftGain,
			float& outRightGain) noexcept;

		static void ApplyVolumetricSpread(
			float distance,
			float volumetricRadius,
			float& inOutLeftGain,
			float& inOutRightGain) noexcept;

		static float CalculateAirAbsorptionCutoff(
			float distance,
			float baseCutoff = 20000.0f,
			float absorptionRate = 0.005f) noexcept;

		static float CalculateDopplerPitch(
			const glm::vec3& sourcePos,
			const glm::vec3& sourceVel,
			const glm::vec3& listenerPos,
			const glm::vec3& listenerVel,
			float speedOfSound = 343.0f,
			float dopplerScale = 1.0f) noexcept;

		static BinauralCues CalculateBinauralCues(
			const glm::vec3& sourcePos,
			const glm::vec3& listenerPos,
			const glm::vec3& listenerRight,
			const glm::vec3& listenerForward = glm::vec3(0.0f, 0.0f, -1.0f),
			const glm::vec3& listenerUp = glm::vec3(0.0f, 1.0f, 0.0f)) noexcept;
	};

	class WLD_API RadioFilter
	{
	public:
		RadioFilter() = default;

		void Configure(
			float hpCutoff = 400.0f,
			float lpCutoff = 3500.0f,
			float distortionDrive = 1.8f,
			float sampleRate = static_cast<float>(kDefaultSampleRate));

		void Reset() noexcept;

		float Process(float inSample) noexcept;
		void ProcessStereo(float& left, float& right) noexcept;
		void Process(float* inOutLeft, float* inOutRight, uint32_t frameCount) noexcept;

		float GetHpCutoff() const noexcept { return m_HpCutoff; }
		float GetLpCutoff() const noexcept { return m_LpCutoff; }
		float GetDistortionDrive() const noexcept { return m_DistortionDrive; }

	private:
		BiquadFilter m_HighPassL;
		BiquadFilter m_HighPassR;
		BiquadFilter m_LowPassL;
		BiquadFilter m_LowPassR;

		float m_HpCutoff { 400.0f };
		float m_LpCutoff { 3500.0f };
		float m_DistortionDrive { 1.8f };
		float m_MakeupGain { 0.85f };
	};

	class WLD_API TypewriterBlipThrottler
	{
	public:
		TypewriterBlipThrottler() = default;

		void Configure(
			float baseIntervalSeconds = 0.040f,
			float punctuationExtraSeconds = 0.080f,
			float pitchVariationSemitones = 0.8f) noexcept;

		void Reset() noexcept;

		// Returns true if audio blip should play for this character
		// outPitchMultiplier returns the adjusted pitch ratio (1.0 = base)
		bool ShouldPlayCharacter(char c, float currentTimeSeconds, float& outPitchMultiplier) noexcept;
		bool ShouldPlayCharacter(char32_t codepoint, float currentTimeSeconds, float& outPitchMultiplier) noexcept;

		float GetBaseInterval() const noexcept { return m_BaseInterval; }
		float GetPunctuationExtra() const noexcept { return m_PunctuationExtra; }

	private:
		static bool IsPunctuation(char32_t c) noexcept;

		float m_BaseInterval { 0.040f };           // 40ms minimum between blips
		float m_PunctuationExtra { 0.080f };       // +80ms pause after punctuation (total 120ms)
		float m_PitchVariationSemitones { 0.8f };  // +/- 0.8 semitone subtle variation
		float m_LastBlipTime { -1.0f };
		bool m_LastWasPunctuation { false };
		uint32_t m_RngSeed { 12345 };
	};

	// 5-band parametric equalizer (LowShelf, LowMid, Mid, HighMid, HighShelf)
	class WLD_API ParametricEq
	{
	public:
		static constexpr size_t kBandCount = 5;

		struct BandSettings
		{
			FilterType type { FilterType::PeakingEQ };
			float frequencyHz { 1000.0f };
			float gainDb { 0.0f };
			float q { 0.7071f };
		};

		ParametricEq();

		void SetBand(size_t bandIndex, float gainDb, float q = 0.7071f) noexcept;
		void SetBandGain(size_t bandIndex, float gainDb) noexcept;
		void SetBandQ(size_t bandIndex, float q) noexcept;
		void SetBandFrequency(size_t bandIndex, float frequencyHz) noexcept;

		const BandSettings& GetBand(size_t bandIndex) const noexcept;
		BandSettings& GetBand(size_t bandIndex) noexcept;

		void Reset() noexcept;
		void Process(float* inOutLeft, float* inOutRight, uint32_t frameCount, float sampleRate);

	private:
		void UpdateFilters(float sampleRate);

		BandSettings m_Bands[kBandCount];
		BiquadFilter m_FiltersL[kBandCount];
		BiquadFilter m_FiltersR[kBandCount];
		float m_LastSampleRate { 0.0f };
		bool m_Dirty { true };
	};

	// Algorithmic Feedback Delay Network (FDN) 8-line reverb
	class WLD_API AlgorithmicReverb
	{
	public:
		static constexpr size_t kNumDelayLines = 8;

		AlgorithmicReverb();

		void Configure(
			float roomSize = 0.5f,
			float damping = 0.5f,
			float preDelayMs = 20.0f,
			float wetGain = 0.3f,
			float dryGain = 1.0f) noexcept;

		void SetRoomSize(float r) noexcept { m_RoomSize = std::clamp(r, 0.0f, 1.0f); m_Dirty = true; }
		void SetDamping(float d) noexcept { m_Damping = std::clamp(d, 0.0f, 1.0f); }
		void SetPreDelayMs(float ms) noexcept { m_PreDelayMs = std::clamp(ms, 0.0f, 200.0f); }
		void SetWetGain(float w) noexcept { m_WetGain = std::clamp(w, 0.0f, 1.0f); }
		void SetDryGain(float d) noexcept { m_DryGain = std::clamp(d, 0.0f, 1.0f); }

		float GetRoomSize() const noexcept { return m_RoomSize; }
		float GetDamping() const noexcept { return m_Damping; }
		float GetPreDelayMs() const noexcept { return m_PreDelayMs; }
		float GetWetGain() const noexcept { return m_WetGain; }
		float GetDryGain() const noexcept { return m_DryGain; }

		void Reset() noexcept;
		void Process(float* inOutLeft, float* inOutRight, uint32_t frameCount, float sampleRate);

	private:
		void UpdateParameters(float sampleRate);

		float m_RoomSize { 0.5f };
		float m_Damping { 0.5f };
		float m_PreDelayMs { 20.0f };
		float m_WetGain { 0.3f };
		float m_DryGain { 1.0f };

		std::vector<float> m_DelayBuffers[kNumDelayLines];
		size_t m_DelayLengths[kNumDelayLines] {};
		size_t m_WriteIndices[kNumDelayLines] {};
		float m_DampedStates[kNumDelayLines] {};

		std::vector<float> m_PreDelayBuffer;
		size_t m_PreDelayWriteIndex { 0 };

		float m_LastSampleRate { 0.0f };
		bool m_Dirty { true };
	};

	// Stereo delay with ping-pong option and feedback control
	class WLD_API StereoDelay
	{
	public:
		StereoDelay();

		void Configure(
			float delayTimeLeftMs = 250.0f,
			float delayTimeRightMs = 375.0f,
			float feedbackGain = 0.4f,
			bool pingPong = false,
			float wetDryMix = 0.5f) noexcept;

		void SetDelayTimeLeft(float ms) noexcept { m_DelayTimeLeftMs = std::clamp(ms, 1.0f, 2000.0f); }
		void SetDelayTimeRight(float ms) noexcept { m_DelayTimeRightMs = std::clamp(ms, 1.0f, 2000.0f); }
		void SetFeedback(float fb) noexcept { m_FeedbackGain = std::clamp(fb, 0.0f, 0.95f); }
		void SetPingPong(bool pingPong) noexcept { m_PingPong = pingPong; }
		void SetWetDryMix(float mix) noexcept { m_WetDryMix = std::clamp(mix, 0.0f, 1.0f); }

		float GetDelayTimeLeft() const noexcept { return m_DelayTimeLeftMs; }
		float GetDelayTimeRight() const noexcept { return m_DelayTimeRightMs; }
		float GetFeedback() const noexcept { return m_FeedbackGain; }
		bool IsPingPong() const noexcept { return m_PingPong; }
		float GetWetDryMix() const noexcept { return m_WetDryMix; }

		void Reset() noexcept;
		void Process(float* inOutLeft, float* inOutRight, uint32_t frameCount, float sampleRate);

	private:
		float m_DelayTimeLeftMs { 250.0f };
		float m_DelayTimeRightMs { 375.0f };
		float m_FeedbackGain { 0.4f };
		bool m_PingPong { false };
		float m_WetDryMix { 0.5f };

		std::vector<float> m_BufferLeft;
		std::vector<float> m_BufferRight;
		size_t m_WritePosLeft { 0 };
		size_t m_WritePosRight { 0 };
		float m_LastSampleRate { 0.0f };
	};

	class WLD_API ConvolutionReverb
	{
	public:
		ConvolutionReverb() = default;

		void LoadImpulseResponse(const AudioBuffer& irBuffer);
		bool HasImpulseResponse() const noexcept { return !m_IrSamples.empty(); }
		size_t GetIrLengthFrames() const noexcept { return m_IrSamples.size(); }

		void SetWetDryMix(float mix01) noexcept { m_WetDryMix = std::clamp(mix01, 0.0f, 1.0f); }
		float GetWetDryMix() const noexcept { return m_WetDryMix; }

		void Reset() noexcept;
		void Process(float* inOutLeft, float* inOutRight, uint32_t frameCount);

	private:
		std::vector<float> m_IrSamples;
		std::vector<float> m_HistoryL;
		std::vector<float> m_HistoryR;
		size_t m_HistoryPos { 0 };
		float m_WetDryMix { 0.4f };
	};

	class WLD_API ProceduralSynth
	{
	public:
		enum class WaveType : uint8_t
		{
			Sine = 0,
			Square = 1,
			Triangle = 2,
			Sawtooth = 3,
			Pulse = 4,
			Noise = 5
		};

		static AudioBuffer GenerateTone(
			WaveType type,
			float freqHz,
			float durationSec,
			float dutyCycle = 0.5f,
			float volume = 0.5f,
			uint32_t sampleRate = kDefaultSampleRate);
	};
}
