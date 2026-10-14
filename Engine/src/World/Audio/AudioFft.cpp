#include "AudioFft.h"
#include <cmath>
#include <numeric>

namespace World::Audio
{
	static constexpr float kPi = 3.14159265358979323846f;

	AudioFftAnalyzer::AudioFftAnalyzer(size_t fftSize)
		: m_FftSize(fftSize)
	{
		// Ensure power of 2 (min 64, max 4096)
		size_t p = 1;
		while (p < m_FftSize) p <<= 1;
		m_FftSize = std::clamp(p, static_cast<size_t>(64), static_cast<size_t>(4096));

		// Precompute Hanning window
		m_Window.resize(m_FftSize);
		for (size_t i = 0; i < m_FftSize; ++i)
		{
			m_Window[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(m_FftSize - 1)));
		}

		m_RingBuffer.assign(m_FftSize, 0.0f);
		m_WriteIndex = 0;
	}

	void AudioFftAnalyzer::Reset()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::fill(m_RingBuffer.begin(), m_RingBuffer.end(), 0.0f);
		m_WriteIndex = 0;
		std::fill(m_SmoothSpectrum.begin(), m_SmoothSpectrum.end(), 0.0f);
	}

	void AudioFftAnalyzer::FeedSamplesStereo(const float* left, const float* right, uint32_t frameCount)
	{
		if (!left || frameCount == 0) return;
		std::lock_guard<std::mutex> lock(m_Mutex);

		for (uint32_t i = 0; i < frameCount; ++i)
		{
			float mono = right ? (left[i] + right[i]) * 0.5f : left[i];
			m_RingBuffer[m_WriteIndex] = mono;
			m_WriteIndex = (m_WriteIndex + 1) % m_FftSize;
		}
	}

	void AudioFftAnalyzer::FeedSamplesMono(const float* samples, uint32_t frameCount)
	{
		if (!samples || frameCount == 0) return;
		std::lock_guard<std::mutex> lock(m_Mutex);

		for (uint32_t i = 0; i < frameCount; ++i)
		{
			m_RingBuffer[m_WriteIndex] = samples[i];
			m_WriteIndex = (m_WriteIndex + 1) % m_FftSize;
		}
	}

	void AudioFftAnalyzer::PerformFft(std::vector<std::complex<float>>& data)
	{
		const size_t n = data.size();

		// Bit-reversal permutation
		for (size_t i = 1, j = 0; i < n; ++i)
		{
			size_t bit = n >> 1;
			for (; j & bit; bit >>= 1)
			{
				j ^= bit;
			}
			j ^= bit;

			if (i < j)
			{
				std::swap(data[i], data[j]);
			}
		}

		// Cooley-Tukey Radix-2 decimation-in-time
		for (size_t len = 2; len <= n; len <<= 1)
		{
			float angle = -2.0f * kPi / static_cast<float>(len);
			std::complex<float> wlen(std::cos(angle), std::sin(angle));

			for (size_t i = 0; i < n; i += len)
			{
				std::complex<float> w(1.0f, 0.0f);
				for (size_t j = 0; j < len / 2; ++j)
				{
					std::complex<float> u = data[i + j];
					std::complex<float> v = data[i + j + len / 2] * w;
					data[i + j] = u + v;
					data[i + j + len / 2] = u - v;
					w *= wlen;
				}
			}
		}
	}

	void AudioFftAnalyzer::GetRawMagnitudes(std::vector<float>& outMagnitudes)
	{
		std::vector<std::complex<float>> fftData(m_FftSize);
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			size_t startIdx = m_WriteIndex;
			for (size_t i = 0; i < m_FftSize; ++i)
			{
				size_t idx = (startIdx + i) % m_FftSize;
				fftData[i] = std::complex<float>(m_RingBuffer[idx] * m_Window[i], 0.0f);
			}
		}

		PerformFft(fftData);

		const size_t halfSize = m_FftSize / 2;
		outMagnitudes.resize(halfSize);
		const float scale = 2.0f / static_cast<float>(m_FftSize);

		for (size_t i = 0; i < halfSize; ++i)
		{
			outMagnitudes[i] = std::abs(fftData[i]) * scale;
		}
	}

	void AudioFftAnalyzer::GetSpectrum(std::vector<float>& outBands, uint32_t bandCount)
	{
		if (bandCount == 0)
		{
			outBands.clear();
			return;
		}

		std::vector<float> magnitudes;
		GetRawMagnitudes(magnitudes);

		const size_t halfSize = magnitudes.size();
		if (halfSize == 0)
		{
			outBands.assign(bandCount, 0.0f);
			return;
		}

		if (m_SmoothSpectrum.size() != bandCount)
		{
			m_SmoothSpectrum.assign(bandCount, 0.0f);
		}

		outBands.resize(bandCount);

		// Logarithmic frequency grouping across 20Hz ~ 20000Hz (at 48kHz)
		const float sampleRate = static_cast<float>(kDefaultSampleRate);
		const float minFreq = 20.0f;
		const float maxFreq = std::min(20000.0f, sampleRate * 0.5f);
		const float freqRatio = maxFreq / minFreq;

		for (uint32_t b = 0; b < bandCount; ++b)
		{
			float f0 = minFreq * std::pow(freqRatio, static_cast<float>(b) / static_cast<float>(bandCount));
			float f1 = minFreq * std::pow(freqRatio, static_cast<float>(b + 1) / static_cast<float>(bandCount));

			size_t k0 = static_cast<size_t>(std::floor(f0 * static_cast<float>(m_FftSize) / sampleRate));
			size_t k1 = static_cast<size_t>(std::ceil(f1 * static_cast<float>(m_FftSize) / sampleRate));

			k0 = std::clamp(k0, static_cast<size_t>(0), halfSize - 1);
			k1 = std::clamp(k1, k0 + 1, halfSize);

			float peakMag = 0.0f;
			for (size_t k = k0; k < k1; ++k)
			{
				peakMag = std::max(peakMag, magnitudes[k]);
			}

			// Normalized amplitude in [0.0, 1.0]
			float curVal = std::clamp(peakMag, 0.0f, 1.0f);

			// Exponential decay falloff smoothing
			m_SmoothSpectrum[b] = std::max(curVal, m_SmoothSpectrum[b] * m_SmoothFactor);
			outBands[b] = m_SmoothSpectrum[b];
		}
	}

	float AudioFftAnalyzer::CalculateBandEnergy(float lowHz, float highHz, float sampleRate)
	{
		if (sampleRate <= 0.0f) sampleRate = static_cast<float>(kDefaultSampleRate);
		std::vector<float> magnitudes;
		GetRawMagnitudes(magnitudes);

		const size_t halfSize = magnitudes.size();
		if (halfSize == 0) return 0.0f;

		size_t k0 = static_cast<size_t>(std::floor(lowHz * static_cast<float>(m_FftSize) / sampleRate));
		size_t k1 = static_cast<size_t>(std::ceil(highHz * static_cast<float>(m_FftSize) / sampleRate));

		k0 = std::clamp(k0, static_cast<size_t>(0), halfSize - 1);
		k1 = std::clamp(k1, k0 + 1, halfSize);

		float sumEnergy = 0.0f;
		for (size_t k = k0; k < k1; ++k)
		{
			sumEnergy += magnitudes[k] * magnitudes[k];
		}

		return sumEnergy / static_cast<float>(k1 - k0);
	}

	// -------------------------------------------------------------
	// BeatDetector Implementation
	// -------------------------------------------------------------

	BeatDetector::BeatDetector(size_t historySize, float sensitivity, float minIntervalSeconds)
		: m_HistorySize(std::max(static_cast<size_t>(8), historySize))
		, m_Sensitivity(sensitivity)
		, m_MinIntervalSeconds(minIntervalSeconds)
	{
		m_EnergyHistory.assign(m_HistorySize, 0.0f);
	}

	void BeatDetector::Reset()
	{
		std::fill(m_EnergyHistory.begin(), m_EnergyHistory.end(), 0.0f);
		m_HistoryIndex = 0;
		m_EnergySum = 0.0f;
		m_FilledCount = 0;
		m_TimeSinceLastBeat = 10.0f;
		m_IsBeat = false;
	}

	bool BeatDetector::Process(float bassEnergy, float deltaSeconds)
	{
		m_TimeSinceLastBeat += deltaSeconds;
		m_IsBeat = false;

		float avgEnergy = (m_FilledCount > 0) ? (m_EnergySum / static_cast<float>(m_FilledCount)) : 0.0f;

		// Trigger condition: bass energy exceeds history average by sensitivity ratio,
		// and cooldown refractory window has passed.
		if (m_FilledCount >= 4 && avgEnergy > 0.0001f)
		{
			if (bassEnergy > avgEnergy * m_Sensitivity && m_TimeSinceLastBeat >= m_MinIntervalSeconds)
			{
				m_IsBeat = true;
				m_TimeSinceLastBeat = 0.0f;
			}
		}

		// Update circular energy history
		m_EnergySum -= m_EnergyHistory[m_HistoryIndex];
		m_EnergyHistory[m_HistoryIndex] = bassEnergy;
		m_EnergySum += bassEnergy;

		m_HistoryIndex = (m_HistoryIndex + 1) % m_HistorySize;
		if (m_FilledCount < m_HistorySize)
		{
			m_FilledCount++;
		}

		return m_IsBeat;
	}

	bool BeatDetector::ProcessSpectrum(const std::vector<float>& spectrum, float deltaSeconds)
	{
		if (spectrum.empty())
		{
			return Process(0.0f, deltaSeconds);
		}

		// Sum first few bass bands (typically 20Hz ~ 200Hz in log spectrum)
		size_t bassBandCount = std::clamp(spectrum.size() / 8, static_cast<size_t>(1), static_cast<size_t>(4));
		float bassSum = 0.0f;
		for (size_t i = 0; i < bassBandCount && i < spectrum.size(); ++i)
		{
			bassSum += spectrum[i] * spectrum[i];
		}

		return Process(bassSum, deltaSeconds);
	}
}
