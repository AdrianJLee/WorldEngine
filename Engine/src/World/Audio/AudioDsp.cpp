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

	// -------------------------------------------------------------
	// RadioFilter Implementation
	// -------------------------------------------------------------

	void RadioFilter::Configure(
		float hpCutoff,
		float lpCutoff,
		float distortionDrive,
		float sampleRate)
	{
		if (sampleRate <= 0.0f) sampleRate = static_cast<float>(kDefaultSampleRate);
		m_HpCutoff = std::clamp(hpCutoff, 50.0f, 4000.0f);
		m_LpCutoff = std::clamp(lpCutoff, m_HpCutoff + 100.0f, sampleRate * 0.49f);
		m_DistortionDrive = std::clamp(distortionDrive, 1.0f, 10.0f);

		m_HighPassL.Configure(FilterType::HighPass, m_HpCutoff, sampleRate, 0.7071f);
		m_HighPassR.Configure(FilterType::HighPass, m_HpCutoff, sampleRate, 0.7071f);
		m_LowPassL.Configure(FilterType::LowPass, m_LpCutoff, sampleRate, 0.7071f);
		m_LowPassR.Configure(FilterType::LowPass, m_LpCutoff, sampleRate, 0.7071f);
		m_MakeupGain = 0.85f;
	}

	void RadioFilter::Reset() noexcept
	{
		m_HighPassL.Reset();
		m_HighPassR.Reset();
		m_LowPassL.Reset();
		m_LowPassR.Reset();
	}

	float RadioFilter::Process(float inSample) noexcept
	{
		float hp = m_HighPassL.Process(inSample);
		float lp = m_LowPassL.Process(hp);
		float driven = lp * m_DistortionDrive;
		return std::tanh(driven) * m_MakeupGain;
	}

	void RadioFilter::ProcessStereo(float& left, float& right) noexcept
	{
		float hpL = m_HighPassL.Process(left);
		float lpL = m_LowPassL.Process(hpL);
		float drivenL = lpL * m_DistortionDrive;
		left = std::tanh(drivenL) * m_MakeupGain;

		float hpR = m_HighPassR.Process(right);
		float lpR = m_LowPassR.Process(hpR);
		float drivenR = lpR * m_DistortionDrive;
		right = std::tanh(drivenR) * m_MakeupGain;
	}

	void RadioFilter::Process(float* inOutLeft, float* inOutRight, uint32_t frameCount) noexcept
	{
		if (!inOutLeft || frameCount == 0) return;
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			float l = inOutLeft[i];
			float r = inOutRight ? inOutRight[i] : l;
			ProcessStereo(l, r);
			inOutLeft[i] = l;
			if (inOutRight) inOutRight[i] = r;
		}
	}

	// -------------------------------------------------------------
	// TypewriterBlipThrottler Implementation
	// -------------------------------------------------------------

	void TypewriterBlipThrottler::Configure(
		float baseIntervalSeconds,
		float punctuationExtraSeconds,
		float pitchVariationSemitones) noexcept
	{
		m_BaseInterval = std::max(0.010f, baseIntervalSeconds);
		m_PunctuationExtra = std::max(0.0f, punctuationExtraSeconds);
		m_PitchVariationSemitones = std::clamp(pitchVariationSemitones, 0.0f, 6.0f);
	}

	void TypewriterBlipThrottler::Reset() noexcept
	{
		m_LastBlipTime = -1.0f;
		m_LastWasPunctuation = false;
		m_RngSeed = 12345;
	}

	bool TypewriterBlipThrottler::IsPunctuation(char32_t c) noexcept
	{
		switch (c)
		{
		case U'.': case U',': case U'!': case U'?':
		case U';': case U':': case U'-': case U'—':
		case U'"': case U'\'':
		case 0x3002: // '。'
		case 0xFF0C: // '，'
		case 0xFF01: // '！'
		case 0xFF1F: // '？'
		case 0xFF1B: // '；'
		case 0xFF1A: // '：'
		case 0x2026: // '…'
			return true;
		default:
			return false;
		}
	}

	bool TypewriterBlipThrottler::ShouldPlayCharacter(char c, float currentTimeSeconds, float& outPitchMultiplier) noexcept
	{
		return ShouldPlayCharacter(static_cast<char32_t>(static_cast<unsigned char>(c)), currentTimeSeconds, outPitchMultiplier);
	}

	bool TypewriterBlipThrottler::ShouldPlayCharacter(char32_t codepoint, float currentTimeSeconds, float& outPitchMultiplier) noexcept
	{
		outPitchMultiplier = 1.0f;

		// Skip whitespace
		if (codepoint == U' ' || codepoint == U'\t' || codepoint == U'\r' || codepoint == U'\n')
		{
			return false;
		}

		bool isPunc = IsPunctuation(codepoint);
		float requiredInterval = m_BaseInterval;
		if (m_LastWasPunctuation)
		{
			requiredInterval += m_PunctuationExtra;
		}

		if (m_LastBlipTime >= 0.0f && (currentTimeSeconds - m_LastBlipTime) < requiredInterval)
		{
			return false;
		}

		m_LastBlipTime = currentTimeSeconds;
		m_LastWasPunctuation = isPunc;

		// Narrative pitch modulation
		if (codepoint == U'?' || codepoint == 0xFF1F)
		{
			outPitchMultiplier = PitchSemitonesToRatio(1.5f);
		}
		else if (codepoint == U'.' || codepoint == 0x3002 || codepoint == U'!' || codepoint == 0xFF01)
		{
			outPitchMultiplier = PitchSemitonesToRatio(-1.2f);
		}
		else
		{
			m_RngSeed = m_RngSeed * 1664525u + 1013904223u;
			float norm = static_cast<float>(m_RngSeed & 0xFFFF) / 65535.0f;
			float semi = (norm * 2.0f - 1.0f) * m_PitchVariationSemitones;
			outPitchMultiplier = PitchSemitonesToRatio(semi);
		}

		return true;
	}

	// =========================================================================
	// ParametricEq Implementation
	// =========================================================================
	ParametricEq::ParametricEq()
	{
		m_Bands[0] = { FilterType::LowShelf, 80.0f, 0.0f, 0.7071f };
		m_Bands[1] = { FilterType::PeakingEQ, 250.0f, 0.0f, 1.0f };
		m_Bands[2] = { FilterType::PeakingEQ, 1000.0f, 0.0f, 1.0f };
		m_Bands[3] = { FilterType::PeakingEQ, 4000.0f, 0.0f, 1.0f };
		m_Bands[4] = { FilterType::HighShelf, 12000.0f, 0.0f, 0.7071f };
	}

	void ParametricEq::SetBand(size_t bandIndex, float gainDb, float q) noexcept
	{
		if (bandIndex >= kBandCount) return;
		m_Bands[bandIndex].gainDb = std::clamp(gainDb, -18.0f, 18.0f);
		m_Bands[bandIndex].q = std::clamp(q, 0.1f, 10.0f);
		m_Dirty = true;
	}

	void ParametricEq::SetBandGain(size_t bandIndex, float gainDb) noexcept
	{
		if (bandIndex >= kBandCount) return;
		m_Bands[bandIndex].gainDb = std::clamp(gainDb, -18.0f, 18.0f);
		m_Dirty = true;
	}

	void ParametricEq::SetBandQ(size_t bandIndex, float q) noexcept
	{
		if (bandIndex >= kBandCount) return;
		m_Bands[bandIndex].q = std::clamp(q, 0.1f, 10.0f);
		m_Dirty = true;
	}

	void ParametricEq::SetBandFrequency(size_t bandIndex, float frequencyHz) noexcept
	{
		if (bandIndex >= kBandCount) return;
		m_Bands[bandIndex].frequencyHz = std::max(10.0f, frequencyHz);
		m_Dirty = true;
	}

	const ParametricEq::BandSettings& ParametricEq::GetBand(size_t bandIndex) const noexcept
	{
		static const BandSettings kDummy{};
		if (bandIndex >= kBandCount) return kDummy;
		return m_Bands[bandIndex];
	}

	ParametricEq::BandSettings& ParametricEq::GetBand(size_t bandIndex) noexcept
	{
		if (bandIndex >= kBandCount) return m_Bands[0];
		m_Dirty = true;
		return m_Bands[bandIndex];
	}

	void ParametricEq::Reset() noexcept
	{
		for (size_t i = 0; i < kBandCount; ++i)
		{
			m_FiltersL[i].Reset();
			m_FiltersR[i].Reset();
		}
	}

	void ParametricEq::UpdateFilters(float sampleRate)
	{
		for (size_t i = 0; i < kBandCount; ++i)
		{
			m_FiltersL[i].Configure(m_Bands[i].type, m_Bands[i].frequencyHz, sampleRate, m_Bands[i].q, m_Bands[i].gainDb);
			m_FiltersR[i].Configure(m_Bands[i].type, m_Bands[i].frequencyHz, sampleRate, m_Bands[i].q, m_Bands[i].gainDb);
		}
		m_LastSampleRate = sampleRate;
		m_Dirty = false;
	}

	void ParametricEq::Process(float* inOutLeft, float* inOutRight, uint32_t frameCount, float sampleRate)
	{
		if (!inOutLeft || !inOutRight || frameCount == 0) return;

		if (m_Dirty || sampleRate != m_LastSampleRate)
		{
			UpdateFilters(sampleRate);
		}

		for (uint32_t f = 0; f < frameCount; ++f)
		{
			float l = inOutLeft[f];
			float r = inOutRight[f];

			for (size_t b = 0; b < kBandCount; ++b)
			{
				l = m_FiltersL[b].Process(l);
				r = m_FiltersR[b].Process(r);
			}

			inOutLeft[f] = l;
			inOutRight[f] = r;
		}
	}

	// =========================================================================
	// AlgorithmicReverb Implementation
	// =========================================================================
	static constexpr uint32_t kBasePrimeDelays[8] = { 1087, 1283, 1511, 1777, 1999, 2287, 2521, 2803 };

	AlgorithmicReverb::AlgorithmicReverb()
	{
		Reset();
	}

	void AlgorithmicReverb::Configure(
		float roomSize,
		float damping,
		float preDelayMs,
		float wetGain,
		float dryGain) noexcept
	{
		m_RoomSize = std::clamp(roomSize, 0.0f, 1.0f);
		m_Damping = std::clamp(damping, 0.0f, 1.0f);
		m_PreDelayMs = std::clamp(preDelayMs, 0.0f, 200.0f);
		m_WetGain = std::clamp(wetGain, 0.0f, 1.0f);
		m_DryGain = std::clamp(dryGain, 0.0f, 1.0f);
		m_Dirty = true;
	}

	void AlgorithmicReverb::Reset() noexcept
	{
		for (size_t i = 0; i < kNumDelayLines; ++i)
		{
			std::fill(m_DelayBuffers[i].begin(), m_DelayBuffers[i].end(), 0.0f);
			m_WriteIndices[i] = 0;
			m_DampedStates[i] = 0.0f;
		}
		std::fill(m_PreDelayBuffer.begin(), m_PreDelayBuffer.end(), 0.0f);
		m_PreDelayWriteIndex = 0;
	}

	void AlgorithmicReverb::UpdateParameters(float sampleRate)
	{
		if (sampleRate <= 0.0f) sampleRate = static_cast<float>(kDefaultSampleRate);
		float scale = (0.6f + m_RoomSize * 0.8f) * (sampleRate / 48000.0f);

		for (size_t i = 0; i < kNumDelayLines; ++i)
		{
			size_t len = static_cast<size_t>(kBasePrimeDelays[i] * scale);
			len = std::max<size_t>(16, len);
			if (m_DelayBuffers[i].size() < len + 16)
			{
				m_DelayBuffers[i].assign(len + 16, 0.0f);
				m_WriteIndices[i] = 0;
			}
			m_DelayLengths[i] = len;
		}

		size_t maxPreDelaySamples = static_cast<size_t>(sampleRate * 0.22f) + 32;
		if (m_PreDelayBuffer.size() < maxPreDelaySamples)
		{
			m_PreDelayBuffer.assign(maxPreDelaySamples, 0.0f);
			m_PreDelayWriteIndex = 0;
		}

		m_LastSampleRate = sampleRate;
		m_Dirty = false;
	}

	void AlgorithmicReverb::Process(float* inOutLeft, float* inOutRight, uint32_t frameCount, float sampleRate)
	{
		if (!inOutLeft || !inOutRight || frameCount == 0) return;

		if (m_Dirty || sampleRate != m_LastSampleRate)
		{
			UpdateParameters(sampleRate);
		}

		size_t preDelaySamples = static_cast<size_t>(m_PreDelayMs * 0.001f * sampleRate);
		size_t preBufSize = m_PreDelayBuffer.empty() ? 1 : m_PreDelayBuffer.size();
		preDelaySamples = std::min(preDelaySamples, preBufSize - 1);

		float feedbackDecay = 0.65f + m_RoomSize * 0.32f;
		float dampCoeff = std::clamp(m_Damping * 0.65f, 0.0f, 0.95f);

		for (uint32_t f = 0; f < frameCount; ++f)
		{
			float inL = inOutLeft[f];
			float inR = inOutRight[f];
			float monoIn = (inL + inR) * 0.5f;

			// Write to pre-delay buffer
			m_PreDelayBuffer[m_PreDelayWriteIndex] = monoIn;
			size_t readPos = (m_PreDelayWriteIndex + preBufSize - preDelaySamples) % preBufSize;
			float delayedInput = m_PreDelayBuffer[readPos];
			m_PreDelayWriteIndex = (m_PreDelayWriteIndex + 1) % preBufSize;

			// Read and damp outputs of the 8 delay lines
			float lineOutputs[kNumDelayLines];
			float sum = 0.0f;
			for (size_t j = 0; j < kNumDelayLines; ++j)
			{
				size_t len = m_DelayLengths[j];
				float rawOut = m_DelayBuffers[j][m_WriteIndices[j]];

				m_DampedStates[j] = m_DampedStates[j] * dampCoeff + rawOut * (1.0f - dampCoeff);
				lineOutputs[j] = m_DampedStates[j];
				sum += lineOutputs[j];
			}

			// Householder orthogonal matrix: H = I - 2/N * 1_N
			float householderOut[kNumDelayLines];
			for (size_t j = 0; j < kNumDelayLines; ++j)
			{
				householderOut[j] = lineOutputs[j] - 0.25f * sum;
			}

			// Write feedback + delayed input into delay lines
			for (size_t j = 0; j < kNumDelayLines; ++j)
			{
				float nextIn = delayedInput + feedbackDecay * householderOut[j];
				nextIn = std::clamp(nextIn, -3.0f, 3.0f);
				m_DelayBuffers[j][m_WriteIndices[j]] = nextIn;
				m_WriteIndices[j] = (m_WriteIndices[j] + 1) % m_DelayLengths[j];
			}

			// Stereophonic tap extraction
			float revL = (lineOutputs[0] - lineOutputs[2] + lineOutputs[4] - lineOutputs[6] + 0.5f * (lineOutputs[1] - lineOutputs[3])) * 0.35f;
			float revR = (lineOutputs[1] - lineOutputs[3] + lineOutputs[5] - lineOutputs[7] + 0.5f * (lineOutputs[0] - lineOutputs[2])) * 0.35f;

			inOutLeft[f] = inL * m_DryGain + revL * m_WetGain;
			inOutRight[f] = inR * m_DryGain + revR * m_WetGain;
		}
	}

	// =========================================================================
	// StereoDelay Implementation
	// =========================================================================
	StereoDelay::StereoDelay()
	{
		Reset();
	}

	void StereoDelay::Configure(
		float delayTimeLeftMs,
		float delayTimeRightMs,
		float feedbackGain,
		bool pingPong,
		float wetDryMix) noexcept
	{
		m_DelayTimeLeftMs = std::clamp(delayTimeLeftMs, 1.0f, 2000.0f);
		m_DelayTimeRightMs = std::clamp(delayTimeRightMs, 1.0f, 2000.0f);
		m_FeedbackGain = std::clamp(feedbackGain, 0.0f, 0.95f);
		m_PingPong = pingPong;
		m_WetDryMix = std::clamp(wetDryMix, 0.0f, 1.0f);
	}

	void StereoDelay::Reset() noexcept
	{
		std::fill(m_BufferLeft.begin(), m_BufferLeft.end(), 0.0f);
		std::fill(m_BufferRight.begin(), m_BufferRight.end(), 0.0f);
		m_WritePosLeft = 0;
		m_WritePosRight = 0;
	}

	void StereoDelay::Process(float* inOutLeft, float* inOutRight, uint32_t frameCount, float sampleRate)
	{
		if (!inOutLeft || !inOutRight || frameCount == 0) return;

		if (sampleRate <= 0.0f) sampleRate = static_cast<float>(kDefaultSampleRate);

		size_t maxCapacity = static_cast<size_t>(sampleRate * 2.05f) + 32;
		if (m_BufferLeft.size() < maxCapacity)
		{
			m_BufferLeft.assign(maxCapacity, 0.0f);
			m_BufferRight.assign(maxCapacity, 0.0f);
			m_WritePosLeft = 0;
			m_WritePosRight = 0;
		}

		size_t cap = m_BufferLeft.size();
		size_t delaySamplesL = std::clamp<size_t>(static_cast<size_t>(m_DelayTimeLeftMs * 0.001f * sampleRate), 1, cap - 1);
		size_t delaySamplesR = std::clamp<size_t>(static_cast<size_t>(m_DelayTimeRightMs * 0.001f * sampleRate), 1, cap - 1);

		for (uint32_t f = 0; f < frameCount; ++f)
		{
			float inL = inOutLeft[f];
			float inR = inOutRight[f];

			size_t readPosL = (m_WritePosLeft + cap - delaySamplesL) % cap;
			size_t readPosR = (m_WritePosRight + cap - delaySamplesR) % cap;

			float delayedL = m_BufferLeft[readPosL];
			float delayedR = m_BufferRight[readPosR];

			float feedbackL = 0.0f;
			float feedbackR = 0.0f;

			if (m_PingPong)
			{
				feedbackL = inL + delayedR * m_FeedbackGain;
				feedbackR = inR + delayedL * m_FeedbackGain;
			}
			else
			{
				feedbackL = inL + delayedL * m_FeedbackGain;
				feedbackR = inR + delayedR * m_FeedbackGain;
			}

			feedbackL = std::clamp(feedbackL, -3.0f, 3.0f);
			feedbackR = std::clamp(feedbackR, -3.0f, 3.0f);

			m_BufferLeft[m_WritePosLeft] = feedbackL;
			m_BufferRight[m_WritePosRight] = feedbackR;

			m_WritePosLeft = (m_WritePosLeft + 1) % cap;
			m_WritePosRight = (m_WritePosRight + 1) % cap;

			inOutLeft[f] = inL * (1.0f - m_WetDryMix) + delayedL * m_WetDryMix;
			inOutRight[f] = inR * (1.0f - m_WetDryMix) + delayedR * m_WetDryMix;
		}
	}

	// =========================================================================
	// ConvolutionReverb Implementation
	// =========================================================================
	void ConvolutionReverb::LoadImpulseResponse(const AudioBuffer& irBuffer)
	{
		uint32_t frames = irBuffer.GetFrameCount();
		uint32_t channels = irBuffer.GetChannels();
		if (frames == 0 || channels == 0)
		{
			m_IrSamples.clear();
			return;
		}

		// Limit maximum IR length to 48000 frames (1 second) for real-time partitioned convolution
		size_t maxFrames = std::min<size_t>(frames, 48000);
		m_IrSamples.resize(maxFrames);

		float maxPeak = 0.0f;
		for (size_t f = 0; f < maxFrames; ++f)
		{
			float mono = 0.0f;
			for (uint32_t c = 0; c < channels; ++c)
			{
				mono += irBuffer.GetSample(c, static_cast<uint32_t>(f));
			}
			mono /= static_cast<float>(channels);
			m_IrSamples[f] = mono;
			maxPeak = std::max(maxPeak, std::abs(mono));
		}

		// Normalize IR peak to 0.707 to avoid clipping
		if (maxPeak > 1e-5f)
		{
			float scale = 0.707f / maxPeak;
			for (float& s : m_IrSamples) s *= scale;
		}

		Reset();
	}

	void ConvolutionReverb::Reset() noexcept
	{
		size_t irLen = m_IrSamples.size();
		if (irLen == 0) irLen = 1;
		m_HistoryL.assign(irLen, 0.0f);
		m_HistoryR.assign(irLen, 0.0f);
		m_HistoryPos = 0;
	}

	void ConvolutionReverb::Process(float* inOutLeft, float* inOutRight, uint32_t frameCount)
	{
		if (!inOutLeft || !inOutRight || frameCount == 0 || m_IrSamples.empty()) return;

		const size_t irLen = m_IrSamples.size();
		if (m_HistoryL.size() < irLen)
		{
			m_HistoryL.assign(irLen, 0.0f);
			m_HistoryR.assign(irLen, 0.0f);
			m_HistoryPos = 0;
		}

		const float dry = 1.0f - m_WetDryMix;
		const float wet = m_WetDryMix;

		for (uint32_t f = 0; f < frameCount; ++f)
		{
			float inL = inOutLeft[f];
			float inR = inOutRight[f];

			m_HistoryL[m_HistoryPos] = inL;
			m_HistoryR[m_HistoryPos] = inR;

			// Direct time-domain convolution with impulse response
			float convL = 0.0f;
			float convR = 0.0f;
			size_t readIndex = m_HistoryPos;

			for (size_t k = 0; k < irLen; ++k)
			{
				float ir = m_IrSamples[k];
				convL += m_HistoryL[readIndex] * ir;
				convR += m_HistoryR[readIndex] * ir;

				readIndex = (readIndex == 0) ? (irLen - 1) : (readIndex - 1);
			}

			inOutLeft[f] = inL * dry + convL * wet;
			inOutRight[f] = inR * dry + convR * wet;

			m_HistoryPos = (m_HistoryPos + 1) % irLen;
		}
	}

	// =========================================================================
	// ProceduralSynth Implementation
	// =========================================================================
	AudioBuffer ProceduralSynth::GenerateTone(
		WaveType type,
		float freqHz,
		float durationSec,
		float dutyCycle,
		float volume,
		uint32_t sampleRate)
	{
		uint32_t frames = static_cast<uint32_t>(std::max(0.01f, durationSec) * static_cast<float>(sampleRate));
		AudioBuffer buf(sampleRate, 2, frames);

		double phase = 0.0;
		constexpr double kTwoPi = 6.28318530717958647692;
		double phaseStep = kTwoPi * static_cast<double>(freqHz) / static_cast<double>(sampleRate);
		dutyCycle = std::clamp(dutyCycle, 0.05f, 0.95f);
		volume = std::clamp(volume, 0.0f, 1.0f);

		for (uint32_t i = 0; i < frames; ++i)
		{
			float val = 0.0f;
			double normPhase = phase / kTwoPi; // [0, 1)

			switch (type)
			{
			case WaveType::Sine:
				val = static_cast<float>(std::sin(phase));
				break;
			case WaveType::Square:
			case WaveType::Pulse:
				val = (normPhase < dutyCycle) ? 1.0f : -1.0f;
				break;
			case WaveType::Triangle:
				val = static_cast<float>(2.0 * std::abs(2.0 * (normPhase - std::floor(normPhase + 0.5))) - 1.0);
				break;
			case WaveType::Sawtooth:
				val = static_cast<float>(2.0 * (normPhase - std::floor(normPhase + 0.5)));
				break;
			case WaveType::Noise:
				val = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.0f - 1.0f;
				break;
			}

			val *= volume;
			buf.SetSample(0, i, val);
			buf.SetSample(1, i, val);

			phase += phaseStep;
			if (phase >= kTwoPi) phase -= kTwoPi;
		}

		return buf;
	}
}
