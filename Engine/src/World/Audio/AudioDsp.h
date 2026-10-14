#pragma once

#include "AudioTypes.h"
#include <cmath>
#include <algorithm>

namespace World::Audio
{
	enum class FilterType : uint8_t
	{
		LowPass = 0,
		HighPass = 1,
		BandPass = 2,
		PeakingEQ = 3
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
	};
}
