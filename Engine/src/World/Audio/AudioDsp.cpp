#include "AudioDsp.h"

namespace World::Audio
{
	void BiquadFilter::Configure(FilterType type, float frequencyHz, float sampleRate, float q, float gainDb)
	{
		if (sampleRate <= 0.0f) sampleRate = static_cast<float>(kDefaultSampleRate);
		float nyquist = sampleRate * 0.5f;
		frequencyHz = std::clamp(frequencyHz, 10.0f, nyquist - 10.0f);
		q = std::max(0.1f, q);

		constexpr float kPi = 3.14159265358979323846f;
		float w0 = 2.0f * kPi * frequencyHz / sampleRate;
		float cosW0 = std::cos(w0);
		float sinW0 = std::sin(w0);
		float alpha = sinW0 / (2.0f * q);

		float a0 = 1.0f;

		switch (type)
		{
		case FilterType::LowPass:
			m_B0 = (1.0f - cosW0) * 0.5f;
			m_B1 = 1.0f - cosW0;
			m_B2 = (1.0f - cosW0) * 0.5f;
			a0 = 1.0f + alpha;
			m_A1 = -2.0f * cosW0;
			m_A2 = 1.0f - alpha;
			break;

		case FilterType::HighPass:
			m_B0 = (1.0f + cosW0) * 0.5f;
			m_B1 = -(1.0f + cosW0);
			m_B2 = (1.0f + cosW0) * 0.5f;
			a0 = 1.0f + alpha;
			m_A1 = -2.0f * cosW0;
			m_A2 = 1.0f - alpha;
			break;

		case FilterType::BandPass:
			m_B0 = alpha;
			m_B1 = 0.0f;
			m_B2 = -alpha;
			a0 = 1.0f + alpha;
			m_A1 = -2.0f * cosW0;
			m_A2 = 1.0f - alpha;
			break;

		case FilterType::PeakingEQ:
			{
				float A = std::pow(10.0f, gainDb / 40.0f);
				m_B0 = 1.0f + alpha * A;
				m_B1 = -2.0f * cosW0;
				m_B2 = 1.0f - alpha * A;
				a0 = 1.0f + alpha / A;
				m_A1 = -2.0f * cosW0;
				m_A2 = 1.0f - alpha / A;
			}
			break;
		}

		// Normalize by a0
		m_B0 /= a0;
		m_B1 /= a0;
		m_B2 /= a0;
		m_A1 /= a0;
		m_A2 /= a0;
	}

	void BiquadFilter::Reset() noexcept
	{
		m_X1 = m_X2 = m_Y1 = m_Y2 = 0.0f;
	}

	float BiquadFilter::Process(float inSample) noexcept
	{
		float outSample = m_B0 * inSample + m_B1 * m_X1 + m_B2 * m_X2 - m_A1 * m_Y1 - m_A2 * m_Y2;
		m_X2 = m_X1;
		m_X1 = inSample;
		m_Y2 = m_Y1;
		m_Y1 = outSample;
		return outSample;
	}

	void DynamicsCompressor::Configure(float thresholdDb, float ratio, float attackMs, float releaseMs, float sampleRate)
	{
		m_ThresholdLinear = DbToLinear(thresholdDb);
		m_Ratio = std::max(1.0f, ratio);
		if (sampleRate <= 0.0f) sampleRate = static_cast<float>(kDefaultSampleRate);

		m_AttackCoeff = std::exp(-1.0f / (attackMs * 0.001f * sampleRate));
		m_ReleaseCoeff = std::exp(-1.0f / (releaseMs * 0.001f * sampleRate));
		m_Envelope = 0.0f;
	}

	void DynamicsCompressor::Reset() noexcept
	{
		m_Envelope = 0.0f;
	}

	void DynamicsCompressor::ProcessStereo(float& left, float& right) noexcept
	{
		float inputPeak = std::max(std::abs(left), std::abs(right));
		if (inputPeak > m_Envelope)
		{
			m_Envelope = m_AttackCoeff * m_Envelope + (1.0f - m_AttackCoeff) * inputPeak;
		}
		else
		{
			m_Envelope = m_ReleaseCoeff * m_Envelope + (1.0f - m_ReleaseCoeff) * inputPeak;
		}

		if (m_Envelope > m_ThresholdLinear && m_Envelope > 0.0001f)
		{
			float overDb = LinearToDb(m_Envelope / m_ThresholdLinear);
			float gainReductionDb = overDb * (1.0f - 1.0f / m_Ratio);
			float gain = DbToLinear(-gainReductionDb);
			left *= gain;
			right *= gain;
		}
	}

	void TruePeakLimiter::Configure(float ceilingDb, float releaseMs, float sampleRate)
	{
		m_CeilingLinear = DbToLinear(ceilingDb);
		if (sampleRate <= 0.0f) sampleRate = static_cast<float>(kDefaultSampleRate);
		m_ReleaseCoeff = std::exp(-1.0f / (releaseMs * 0.001f * sampleRate));
		m_Gain = 1.0f;
	}

	void TruePeakLimiter::Reset() noexcept
	{
		m_Gain = 1.0f;
	}

	void TruePeakLimiter::ProcessStereo(float& left, float& right) noexcept
	{
		float peak = std::max(std::abs(left), std::abs(right));
		if (peak > 0.00001f)
		{
			float targetGain = (peak > m_CeilingLinear) ? (m_CeilingLinear / peak) : 1.0f;
			if (targetGain < m_Gain)
			{
				m_Gain = targetGain; // Fast attack
			}
			else
			{
				m_Gain = m_ReleaseCoeff * m_Gain + (1.0f - m_ReleaseCoeff) * targetGain;
			}
		}

		left *= m_Gain;
		right *= m_Gain;

		// Hard safety clamp to prevent DAC wrap
		left = std::clamp(left, -1.0f, 1.0f);
		right = std::clamp(right, -1.0f, 1.0f);
	}

	void Bitcrusher::Configure(int bitDepth, int downsampleFactor) noexcept
	{
		m_BitDepth = std::clamp(bitDepth, 2, 16);
		m_DownsampleFactor = std::clamp(downsampleFactor, 1, 32);
		Reset();
	}

	void Bitcrusher::Reset() noexcept
	{
		m_DownsampleCounter = 0;
		m_HeldLeft = 0.0f;
		m_HeldRight = 0.0f;
	}

	void Bitcrusher::ProcessSample(float& inOutLeft, float& inOutRight) noexcept
	{
		if (m_DownsampleCounter == 0)
		{
			float steps = static_cast<float>(1 << (m_BitDepth - 1));
			float invSteps = 1.0f / steps;

			float l = std::clamp(inOutLeft, -1.0f, 1.0f);
			m_HeldLeft = std::round(l * steps) * invSteps;

			float r = std::clamp(inOutRight, -1.0f, 1.0f);
			m_HeldRight = std::round(r * steps) * invSteps;
		}

		inOutLeft = m_HeldLeft;
		inOutRight = m_HeldRight;

		if (++m_DownsampleCounter >= m_DownsampleFactor)
		{
			m_DownsampleCounter = 0;
		}
	}

	void Bitcrusher::Process(float* inOutLeft, float* inOutRight, uint32_t frameCount) noexcept
	{
		if (frameCount == 0) return;
		float steps = static_cast<float>(1 << (m_BitDepth - 1));
		float invSteps = 1.0f / steps;

		for (uint32_t i = 0; i < frameCount; ++i)
		{
			if (m_DownsampleCounter == 0)
			{
				if (inOutLeft)
				{
					float l = std::clamp(inOutLeft[i], -1.0f, 1.0f);
					m_HeldLeft = std::round(l * steps) * invSteps;
				}
				if (inOutRight)
				{
					float r = std::clamp(inOutRight[i], -1.0f, 1.0f);
					m_HeldRight = std::round(r * steps) * invSteps;
				}
			}

			if (inOutLeft) inOutLeft[i] = m_HeldLeft;
			if (inOutRight) inOutRight[i] = m_HeldRight;

			if (++m_DownsampleCounter >= m_DownsampleFactor)
			{
				m_DownsampleCounter = 0;
			}
		}
	}

	float SpatialCalculator::CalculateDistanceAttenuation(
		float distance,
		float minDistance,
		float maxDistance,
		AttenuationModel model) noexcept
	{
		if (distance <= minDistance) return 1.0f;
		if (distance >= maxDistance) return 0.0f;

		float range = maxDistance - minDistance;
		if (range <= 0.001f) return 0.0f;

		float normDist = (distance - minDistance) / range; // 0.0 ~ 1.0

		switch (model)
		{
		case AttenuationModel::Linear:
			return 1.0f - normDist;

		case AttenuationModel::Logarithmic:
			// Smooth logarithmic rolloff
			return std::clamp(1.0f - std::log10(1.0f + 9.0f * normDist), 0.0f, 1.0f);

		case AttenuationModel::InverseSquare:
			{
				float distClamped = std::max(distance, minDistance);
				return (minDistance * minDistance) / (distClamped * distClamped);
			}

		case AttenuationModel::CustomCurve:
		default:
			// S-Curve smoothstep
			return 1.0f - (normDist * normDist * (3.0f - 2.0f * normDist));
		}
	}

	float SpatialCalculator::CalculateConeAttenuation(
		const glm::vec3& sourcePos,
		const glm::vec3& sourceForward,
		const glm::vec3& listenerPos,
		float innerAngleDeg,
		float outerAngleDeg,
		float outerGain) noexcept
	{
		glm::vec3 toListener = listenerPos - sourcePos;
		float dist = glm::length(toListener);
		if (dist < 0.0001f) return 1.0f;

		toListener /= dist;
		float dotVal = glm::clamp(glm::dot(sourceForward, toListener), -1.0f, 1.0f);
		float angleDeg = std::acos(dotVal) * 57.295779513f; // rad to deg

		float halfInner = innerAngleDeg * 0.5f;
		float halfOuter = outerAngleDeg * 0.5f;

		if (angleDeg <= halfInner) return 1.0f;
		if (angleDeg >= halfOuter) return outerGain;

		float t = (angleDeg - halfInner) / (halfOuter - halfInner);
		return 1.0f + t * (outerGain - 1.0f);
	}

	void SpatialCalculator::CalculateStereoPan(
		const glm::vec3& sourcePos,
		const glm::vec3& listenerPos,
		const glm::vec3& listenerRight,
		float& outLeftGain,
		float& outRightGain) noexcept
	{
		glm::vec3 toSource = sourcePos - listenerPos;
		float dist = glm::length(toSource);
		if (dist < 0.0001f)
		{
			outLeftGain = 0.7071f;
			outRightGain = 0.7071f;
			return;
		}

		toSource /= dist;
		float pan = glm::clamp(glm::dot(toSource, listenerRight), -1.0f, 1.0f);

		// Equal power panning: left = cos(theta), right = sin(theta)
		constexpr float kPiOver4 = 0.7853981633974483f;
		float angle = (pan + 1.0f) * kPiOver4; // 0.0 ~ pi/2
		outLeftGain = std::cos(angle);
		outRightGain = std::sin(angle);
	}

	void SpatialCalculator::ApplyVolumetricSpread(
		float distance,
		float volumetricRadius,
		float& inOutLeftGain,
		float& inOutRightGain) noexcept
	{
		if (volumetricRadius <= 0.0001f || distance >= volumetricRadius)
		{
			return;
		}

		float spread = std::clamp(1.0f - distance / std::max(volumetricRadius, 0.001f), 0.0f, 1.0f);
		constexpr float kCenterGain = 0.70710678f;
		inOutLeftGain = (1.0f - spread) * inOutLeftGain + spread * kCenterGain;
		inOutRightGain = (1.0f - spread) * inOutRightGain + spread * kCenterGain;
	}

	float SpatialCalculator::CalculateAirAbsorptionCutoff(
		float distance,
		float baseCutoff,
		float absorptionRate) noexcept
	{
		if (distance <= 0.0f) return baseCutoff;
		float cutoff = baseCutoff * std::exp(-distance * absorptionRate);
		return std::clamp(cutoff, 800.0f, 20000.0f);
	}

	float SpatialCalculator::CalculateDopplerPitch(
		const glm::vec3& sourcePos,
		const glm::vec3& sourceVel,
		const glm::vec3& listenerPos,
		const glm::vec3& listenerVel,
		float speedOfSound,
		float dopplerScale) noexcept
	{
		glm::vec3 r = sourcePos - listenerPos;
		float dist = glm::length(r);
		if (dist < 0.0001f) return 1.0f;

		glm::vec3 dir = r / dist;
		float vl = glm::dot(listenerVel, dir) * dopplerScale;
		float vs = glm::dot(sourceVel, dir) * dopplerScale;

		float c = std::max(speedOfSound, 1.0f);
		float num = c + vl;
		float den = c + vs;

		if (den < 0.1f * c) den = 0.1f * c;
		if (num < 0.1f * c) num = 0.1f * c;

		float ratio = num / den;
		return std::clamp(ratio, 0.25f, 4.0f);
	}

	BinauralCues SpatialCalculator::CalculateBinauralCues(
		const glm::vec3& sourcePos,
		const glm::vec3& listenerPos,
		const glm::vec3& listenerRight,
		const glm::vec3& listenerForward,
		const glm::vec3& listenerUp) noexcept
	{
		BinauralCues cues{};
		CalculateStereoPan(sourcePos, listenerPos, listenerRight, cues.leftGain, cues.rightGain);

		glm::vec3 toSource = sourcePos - listenerPos;
		float dist = glm::length(toSource);
		if (dist < 0.0001f)
		{
			return cues;
		}

		glm::vec3 dir = toSource / dist;
		float dotFwd = glm::dot(dir, listenerForward);
		float dotUp = glm::dot(dir, listenerUp);

		cues.elevationFactor = std::clamp(dotUp, -1.0f, 1.0f);

		if (dotFwd < 0.0f)
		{
			float rearFactor = std::clamp(-dotFwd, 0.0f, 1.0f);
			cues.rearFilterCutoff = 20000.0f * (1.0f - rearFactor * 0.675f);
			constexpr float kMaxRearDb = -4.0f;
			float minGain = DbToLinear(kMaxRearDb);
			cues.rearGain = (1.0f - rearFactor) * 1.0f + rearFactor * minGain;
		}
		else
		{
			cues.rearFilterCutoff = 20000.0f;
			cues.rearGain = 1.0f;
		}

		return cues;
	}
}
