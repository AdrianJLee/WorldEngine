#pragma once

#include "AudioTypes.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace World::Audio
{
	class WLD_API AudioBuffer
	{
	public:
		AudioBuffer() = default;
		AudioBuffer(uint32_t sampleRate, uint32_t channels, uint32_t frameCount);

		void Resize(uint32_t sampleRate, uint32_t channels, uint32_t frameCount);
		void Clear();

		uint32_t GetSampleRate() const noexcept { return m_SampleRate; }
		uint32_t GetChannels() const noexcept { return m_Channels; }
		uint32_t GetFrameCount() const noexcept { return m_FrameCount; }
		float GetDurationSeconds() const noexcept;

		const std::vector<float>& GetSamples() const noexcept { return m_Samples; }
		std::vector<float>& GetSamples() noexcept { return m_Samples; }

		float GetSample(uint32_t channel, uint32_t frame) const noexcept;
		void SetSample(uint32_t channel, uint32_t frame, float value) noexcept;

		void SetLoopPoints(uint32_t startFrame, uint32_t endFrame) noexcept;
		uint32_t GetLoopStartFrame() const noexcept { return m_LoopStartFrame; }
		uint32_t GetLoopEndFrame() const noexcept { return m_LoopEndFrame; }
		bool HasLoopPoints() const noexcept { return m_LoopEndFrame > m_LoopStartFrame; }

		float CalculatePeak() const noexcept;
		float CalculateRms() const noexcept;
		float EstimateLufs() const noexcept;
		void NormalizeLoudness(float targetLufs = -16.0f) noexcept;

		// Generator helpers for testing and synthetic cues
		static AudioBuffer GenerateSineWave(float freqHz, float durationSec, float amplitude = 0.5f, uint32_t sampleRate = kDefaultSampleRate);
		static AudioBuffer GenerateWhiteNoise(float durationSec, float amplitude = 0.2f, uint32_t sampleRate = kDefaultSampleRate);

	private:
		uint32_t m_SampleRate { kDefaultSampleRate };
		uint32_t m_Channels { 2 };
		uint32_t m_FrameCount { 0 };
		uint32_t m_LoopStartFrame { 0 };
		uint32_t m_LoopEndFrame { 0 };
		std::vector<float> m_Samples; // Interleaved float samples [-1.0f, 1.0f]
	};
}
