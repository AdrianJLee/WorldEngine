#pragma once

#include "AudioTypes.h"
#include <vector>
#include <complex>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <algorithm>

namespace World::Audio
{
	class WLD_API AudioFftAnalyzer
	{
	public:
		explicit AudioFftAnalyzer(size_t fftSize = 1024);
		~AudioFftAnalyzer() = default;

		// Feed incoming audio frames (thread-safe, called at end of mix callback)
		void FeedSamplesStereo(const float* left, const float* right, uint32_t frameCount);
		void FeedSamplesMono(const float* samples, uint32_t frameCount);

		// Compute current spectrum and map to bandCount output energy bins (e.g. 64 or 256)
		// Normalized values [0.0, 1.0]
		void GetSpectrum(std::vector<float>& outBands, uint32_t bandCount = 64);

		// Raw half-spectrum magnitudes of size fftSize / 2
		void GetRawMagnitudes(std::vector<float>& outMagnitudes);

		// Calculate energy in frequency range [lowHz, highHz]
		float CalculateBandEnergy(float lowHz, float highHz, float sampleRate = static_cast<float>(kDefaultSampleRate));

		size_t GetFftSize() const noexcept { return m_FftSize; }
		void SetSmoothingFactor(float factor) noexcept { m_SmoothFactor = std::clamp(factor, 0.0f, 0.99f); }
		float GetSmoothingFactor() const noexcept { return m_SmoothFactor; }

		void Reset();

	private:
		void PerformFft(std::vector<std::complex<float>>& data);

		size_t m_FftSize { 1024 };
		std::vector<float> m_Window;           // Hanning window table
		std::vector<float> m_RingBuffer;       // Circular sample buffer
		size_t m_WriteIndex { 0 };
		std::vector<float> m_SmoothSpectrum;   // Visual smoothed spectrum
		float m_SmoothFactor { 0.65f };        // Visual falloff smoothing

		mutable std::mutex m_Mutex;
	};

	class WLD_API BeatDetector
	{
	public:
		explicit BeatDetector(size_t historySize = 43, float sensitivity = 1.5f, float minIntervalSeconds = 0.15f);
		~BeatDetector() = default;

		// Feed low-frequency / bass energy and elapsed time
		bool Process(float bassEnergy, float deltaSeconds);

		// Feed complete spectrum and elapsed time (analyzes lowest bands 20~200Hz)
		bool ProcessSpectrum(const std::vector<float>& spectrum, float deltaSeconds);

		bool IsBeat() const noexcept { return m_IsBeat; }
		float GetTimeSinceLastBeat() const noexcept { return m_TimeSinceLastBeat; }

		void SetSensitivity(float factor) noexcept { m_Sensitivity = std::max(1.05f, factor); }
		float GetSensitivity() const noexcept { return m_Sensitivity; }

		void SetMinInterval(float seconds) noexcept { m_MinIntervalSeconds = std::max(0.02f, seconds); }
		float GetMinInterval() const noexcept { return m_MinIntervalSeconds; }

		void Reset();

	private:
		size_t m_HistorySize { 43 }; // ~1 second history at ~43 updates/sec
		float m_Sensitivity { 1.5f };
		float m_MinIntervalSeconds { 0.15f };
		float m_TimeSinceLastBeat { 10.0f };
		bool m_IsBeat { false };

		std::vector<float> m_EnergyHistory;
		size_t m_HistoryIndex { 0 };
		float m_EnergySum { 0.0f };
		size_t m_FilledCount { 0 };
	};
}
