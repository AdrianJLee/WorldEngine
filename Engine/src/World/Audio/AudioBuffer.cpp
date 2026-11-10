#include "AudioBuffer.h"
#include <limits>

namespace World::Audio
{
	AudioBuffer::AudioBuffer(uint32_t sampleRate, uint32_t channels, uint32_t frameCount)
	{
		Resize(sampleRate, channels, frameCount);
	}

	void AudioBuffer::Resize(uint32_t sampleRate, uint32_t channels, uint32_t frameCount)
	{
		m_SampleRate = (sampleRate > 0) ? sampleRate : kDefaultSampleRate;
		m_Channels = (channels > 0) ? channels : 2;
		m_FrameCount = frameCount;
		m_Samples.assign(static_cast<size_t>(m_Channels) * m_FrameCount, 0.0f);
		m_LoopStartFrame = 0;
		m_LoopEndFrame = 0;
	}

	void AudioBuffer::Clear()
	{
		std::fill(m_Samples.begin(), m_Samples.end(), 0.0f);
	}

	float AudioBuffer::GetDurationSeconds() const noexcept
	{
		if (m_SampleRate == 0) return 0.0f;
		return static_cast<float>(m_FrameCount) / static_cast<float>(m_SampleRate);
	}

	float AudioBuffer::GetSample(uint32_t channel, uint32_t frame) const noexcept
	{
		if (channel >= m_Channels || frame >= m_FrameCount) return 0.0f;
		return m_Samples[static_cast<size_t>(frame) * m_Channels + channel];
	}

	void AudioBuffer::SetSample(uint32_t channel, uint32_t frame, float value) noexcept
	{
		if (channel >= m_Channels || frame >= m_FrameCount) return;
		m_Samples[static_cast<size_t>(frame) * m_Channels + channel] = value;
	}

	void AudioBuffer::SetLoopPoints(uint32_t startFrame, uint32_t endFrame) noexcept
	{
		m_LoopStartFrame = std::min(startFrame, m_FrameCount);
		m_LoopEndFrame = std::min(endFrame, m_FrameCount);
	}

	uint32_t AudioBuffer::FindZeroCrossing(uint32_t targetFrame, uint32_t searchWindow) const noexcept
	{
		if (m_FrameCount < 2 || m_Samples.empty())
		{
			return targetFrame;
		}

		targetFrame = std::min(targetFrame, m_FrameCount - 1);
		int64_t tf = static_cast<int64_t>(targetFrame);
		int64_t window = static_cast<int64_t>(searchWindow);

		int64_t startF = std::max<int64_t>(1, tf - window);
		int64_t endF = std::min<int64_t>(static_cast<int64_t>(m_FrameCount) - 1, tf + window);

		uint32_t bestFrame = targetFrame;
		int64_t bestDist = std::numeric_limits<int64_t>::max();
		float bestAbs = std::numeric_limits<float>::max();

		for (int64_t f = startF; f <= endF; ++f)
		{
			float s0 = GetSample(0, static_cast<uint32_t>(f - 1));
			float s1 = GetSample(0, static_cast<uint32_t>(f));

			// Positive-slope zero crossing: s[f-1] <= 0.0f and s[f] >= 0.0f
			if (s0 <= 0.0f && s1 >= 0.0f)
			{
				int64_t dist = std::abs(f - tf);
				float absVal = std::abs(s1);

				if (dist < bestDist || (dist == bestDist && absVal < bestAbs))
				{
					bestDist = dist;
					bestAbs = absVal;
					bestFrame = static_cast<uint32_t>(f);
				}
			}
		}

		return bestFrame;
	}

	void AudioBuffer::SnapLoopPointsToZeroCrossing(uint32_t searchWindow) noexcept
	{
		if (m_FrameCount < 2) return;

		uint32_t targetStart = m_LoopStartFrame;
		uint32_t targetEnd = (m_LoopEndFrame > m_LoopStartFrame) ? m_LoopEndFrame : m_FrameCount;

		m_LoopStartFrame = FindZeroCrossing(targetStart, searchWindow);
		m_LoopEndFrame = FindZeroCrossing(targetEnd, searchWindow);

		if (m_LoopEndFrame <= m_LoopStartFrame)
		{
			m_LoopEndFrame = std::min(m_FrameCount, m_LoopStartFrame + 1);
		}
	}

	float AudioBuffer::CalculatePeak() const noexcept
	{
		float peak = 0.0f;
		for (float s : m_Samples)
		{
			peak = std::max(peak, std::abs(s));
		}
		return peak;
	}

	float AudioBuffer::CalculateRms() const noexcept
	{
		if (m_Samples.empty()) return 0.0f;
		double sumSquares = 0.0;
		for (float s : m_Samples)
		{
			sumSquares += static_cast<double>(s) * static_cast<double>(s);
		}
		return static_cast<float>(std::sqrt(sumSquares / static_cast<double>(m_Samples.size())));
	}

	float AudioBuffer::EstimateLufs() const noexcept
	{
		float rms = CalculateRms();
		if (rms < 0.000001f) return -96.0f;
		// Standard approx: LUFS ~= 20*log10(RMS) - 0.69dB for pink/broadband
		return 20.0f * std::log10(rms) - 0.691f;
	}

	void AudioBuffer::NormalizeLoudness(float targetLufs) noexcept
	{
		float currentLufs = EstimateLufs();
		if (currentLufs <= -90.0f) return;
		float gainDb = targetLufs - currentLufs;
		float linearGain = DbToLinear(gainDb);
		for (float& s : m_Samples)
		{
			s = std::clamp(s * linearGain, -1.0f, 1.0f);
		}
	}

	AudioBuffer AudioBuffer::GenerateSineWave(float freqHz, float durationSec, float amplitude, uint32_t sampleRate)
	{
		uint32_t frames = static_cast<uint32_t>(durationSec * static_cast<float>(sampleRate));
		AudioBuffer buf(sampleRate, 2, frames);
		constexpr double kTwoPi = 6.28318530717958647692;
		double phaseStep = kTwoPi * static_cast<double>(freqHz) / static_cast<double>(sampleRate);
		double phase = 0.0;
		for (uint32_t i = 0; i < frames; ++i)
		{
			float val = static_cast<float>(std::sin(phase)) * amplitude;
			buf.SetSample(0, i, val);
			buf.SetSample(1, i, val);
			phase += phaseStep;
			if (phase >= kTwoPi) phase -= kTwoPi;
		}
		return buf;
	}

	AudioBuffer AudioBuffer::GenerateWhiteNoise(float durationSec, float amplitude, uint32_t sampleRate)
	{
		uint32_t frames = static_cast<uint32_t>(durationSec * static_cast<float>(sampleRate));
		AudioBuffer buf(sampleRate, 2, frames);
		uint32_t seed = 123456789;
		for (uint32_t i = 0; i < frames; ++i)
		{
			seed = (seed * 1103515245 + 12345) & 0x7fffffff;
			float r = (static_cast<float>(seed) / 2147483648.0f) * 2.0f - 1.0f;
			float val = r * amplitude;
			buf.SetSample(0, i, val);
			buf.SetSample(1, i, val);
		}
		return buf;
	}
}