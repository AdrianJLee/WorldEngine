#include "AudioCapture.h"
#include <cmath>
#include <algorithm>

namespace World::Audio
{
	// -------------------------------------------------------------
	// NullAudioCaptureBackend Implementation
	// -------------------------------------------------------------

	bool NullAudioCaptureBackend::Initialize(uint32_t sampleRate, uint32_t channels)
	{
		m_SampleRate = sampleRate;
		m_Channels = channels;
		m_Capturing = false;
		return true;
	}

	bool NullAudioCaptureBackend::Start(CaptureCallback callback)
	{
		m_Callback = std::move(callback);
		m_Capturing = true;
		return true;
	}

	void NullAudioCaptureBackend::Stop()
	{
		m_Capturing = false;
		m_Callback = nullptr;
	}

	void NullAudioCaptureBackend::SimulateMicInput(const float* samples, uint32_t frameCount)
	{
		if (m_Capturing && m_Callback && samples && frameCount > 0)
		{
			m_Callback(samples, frameCount);
		}
	}

	// -------------------------------------------------------------
	// AudioCaptureDevice Implementation
	// -------------------------------------------------------------

	AudioCaptureDevice::AudioCaptureDevice() = default;

	AudioCaptureDevice::~AudioCaptureDevice()
	{
		Shutdown();
	}

	bool AudioCaptureDevice::Initialize(
		std::unique_ptr<IAudioCaptureBackend> backend,
		uint32_t sampleRate,
		uint32_t channels,
		size_t ringBufferCapacityFrames)
	{
		Shutdown();

		m_SampleRate = sampleRate;
		m_Channels = std::max(1u, channels);
		m_Capacity = std::max(static_cast<size_t>(1024), ringBufferCapacityFrames);

		m_InputRingBuffer.assign(m_Capacity, 0.0f);
		m_WriteIndex.store(0, std::memory_order_relaxed);
		m_ReadIndex.store(0, std::memory_order_relaxed);
		m_LastInputRms.store(0.0f, std::memory_order_relaxed);

		if (!backend)
		{
			backend = std::make_unique<NullAudioCaptureBackend>();
		}
		m_Backend = std::move(backend);

		return m_Backend->Initialize(m_SampleRate, m_Channels);
	}

	void AudioCaptureDevice::Shutdown()
	{
		Stop();
		m_Backend.reset();
		ClearBuffer();
	}

	bool AudioCaptureDevice::Start()
	{
		if (!m_Backend) return false;
		ClearBuffer();
		return m_Backend->Start([this](const float* samples, uint32_t frameCount) {
			this->PushSamples(samples, frameCount);
		});
	}

	void AudioCaptureDevice::Stop()
	{
		if (m_Backend && m_Backend->IsCapturing())
		{
			m_Backend->Stop();
		}
	}

	bool AudioCaptureDevice::IsCapturing() const noexcept
	{
		return m_Backend ? m_Backend->IsCapturing() : false;
	}

	void AudioCaptureDevice::PushSamples(const float* samples, uint32_t frameCount)
	{
		if (!samples || frameCount == 0) return;

		// Calculate RMS level for VAD / loudness meter
		float sumSq = 0.0f;
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			float s = samples[i];
			sumSq += s * s;
		}
		float rms = std::sqrt(sumSq / static_cast<float>(frameCount));
		m_LastInputRms.store(rms, std::memory_order_relaxed);

		// Write samples to circular buffer
		std::lock_guard<std::mutex> lock(m_BufferMutex);
		size_t writeIdx = m_WriteIndex.load(std::memory_order_relaxed);
		size_t readIdx = m_ReadIndex.load(std::memory_order_relaxed);

		for (uint32_t i = 0; i < frameCount; ++i)
		{
			m_InputRingBuffer[writeIdx] = samples[i];
			writeIdx = (writeIdx + 1) % m_Capacity;
			if (writeIdx == readIdx)
			{
				// Advance read pointer on overflow (drop oldest)
				readIdx = (readIdx + 1) % m_Capacity;
			}
		}

		m_WriteIndex.store(writeIdx, std::memory_order_release);
		m_ReadIndex.store(readIdx, std::memory_order_release);
	}

	uint32_t AudioCaptureDevice::PopSamples(float* outSamples, uint32_t maxFrames)
	{
		if (!outSamples || maxFrames == 0) return 0;

		std::lock_guard<std::mutex> lock(m_BufferMutex);
		size_t writeIdx = m_WriteIndex.load(std::memory_order_acquire);
		size_t readIdx = m_ReadIndex.load(std::memory_order_relaxed);

		size_t avail = (writeIdx + m_Capacity - readIdx) % m_Capacity;
		size_t count = std::min(avail, static_cast<size_t>(maxFrames));

		for (size_t i = 0; i < count; ++i)
		{
			outSamples[i] = m_InputRingBuffer[readIdx];
			readIdx = (readIdx + 1) % m_Capacity;
		}

		m_ReadIndex.store(readIdx, std::memory_order_release);
		return static_cast<uint32_t>(count);
	}

	uint32_t AudioCaptureDevice::GetAvailableFrames() const noexcept
	{
		size_t writeIdx = m_WriteIndex.load(std::memory_order_acquire);
		size_t readIdx = m_ReadIndex.load(std::memory_order_relaxed);
		return static_cast<uint32_t>((writeIdx + m_Capacity - readIdx) % m_Capacity);
	}

	void AudioCaptureDevice::ClearBuffer() noexcept
	{
		std::lock_guard<std::mutex> lock(m_BufferMutex);
		m_WriteIndex.store(0, std::memory_order_relaxed);
		m_ReadIndex.store(0, std::memory_order_relaxed);
		std::fill(m_InputRingBuffer.begin(), m_InputRingBuffer.end(), 0.0f);
		m_LastInputRms.store(0.0f, std::memory_order_relaxed);
	}
}
