#include "AudioNullBackend.h"

namespace World::Audio
{
	AudioNullBackend::~AudioNullBackend()
	{
		Shutdown();
	}

	bool AudioNullBackend::Initialize(uint32_t sampleRate, uint32_t bufferFrames)
	{
		m_SampleRate = (sampleRate > 0) ? sampleRate : kDefaultSampleRate;
		m_BufferFrames = (bufferFrames > 0) ? bufferFrames : kDefaultMixBlockFrames;
		m_ScratchLeft.assign(m_BufferFrames, 0.0f);
		m_ScratchRight.assign(m_BufferFrames, 0.0f);
		m_Initialized = true;
		m_Running = false;
		return true;
	}

	void AudioNullBackend::Shutdown()
	{
		Stop();
		m_Initialized = false;
		m_CapturedLeft.clear();
		m_CapturedRight.clear();
	}

	bool AudioNullBackend::Start()
	{
		if (!m_Initialized) return false;
		m_Running = true;
		return true;
	}

	void AudioNullBackend::Stop()
	{
		m_Running = false;
	}

	void AudioNullBackend::StepMix(uint32_t frameCount)
	{
		if (!m_Initialized || !m_Callback) return;

		if (m_ScratchLeft.size() < frameCount)
		{
			m_ScratchLeft.assign(frameCount, 0.0f);
			m_ScratchRight.assign(frameCount, 0.0f);
		}
		else
		{
			std::fill(m_ScratchLeft.begin(), m_ScratchLeft.begin() + frameCount, 0.0f);
			std::fill(m_ScratchRight.begin(), m_ScratchRight.begin() + frameCount, 0.0f);
		}

		m_Callback(m_ScratchLeft.data(), m_ScratchRight.data(), frameCount);

		// Append to captured buffer
		m_CapturedLeft.insert(m_CapturedLeft.end(), m_ScratchLeft.begin(), m_ScratchLeft.begin() + frameCount);
		m_CapturedRight.insert(m_CapturedRight.end(), m_ScratchRight.begin(), m_ScratchRight.begin() + frameCount);
	}

	void AudioNullBackend::StepSeconds(float seconds)
	{
		if (seconds <= 0.0f) return;
		uint32_t totalFrames = static_cast<uint32_t>(seconds * static_cast<float>(m_SampleRate));
		uint32_t rendered = 0;
		while (rendered < totalFrames)
		{
			uint32_t batch = std::min(m_BufferFrames, totalFrames - rendered);
			StepMix(batch);
			rendered += batch;
		}
	}

	void AudioNullBackend::ClearCapture() noexcept
	{
		m_CapturedLeft.clear();
		m_CapturedRight.clear();
	}

	AudioBuffer AudioNullBackend::ExportCapturedBuffer() const
	{
		uint32_t count = static_cast<uint32_t>(m_CapturedLeft.size());
		AudioBuffer buf(m_SampleRate, 2, count);
		for (uint32_t i = 0; i < count; ++i)
		{
			buf.SetSample(0, i, m_CapturedLeft[i]);
			buf.SetSample(1, i, m_CapturedRight[i]);
		}
		return buf;
	}
}
