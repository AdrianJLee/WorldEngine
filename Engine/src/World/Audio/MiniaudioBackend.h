#pragma once

#include "IAudioBackend.h"
#include <thread>
#include <atomic>
#include <vector>

namespace World::Audio
{
	class WLD_API MiniaudioBackend final : public IAudioBackend
	{
	public:
		MiniaudioBackend();
		~MiniaudioBackend() override;

		bool Initialize(uint32_t sampleRate, uint32_t bufferFrames) override;
		void Shutdown() override;
		bool IsInitialized() const noexcept override { return m_Initialized; }

		void SetRenderCallback(AudioRenderCallback callback) override { m_Callback = std::move(callback); }
		bool Start() override;
		void Stop() override;

		uint32_t GetSampleRate() const noexcept override { return m_SampleRate; }
		uint32_t GetBufferFrames() const noexcept override { return m_BufferFrames; }

	private:
		void AudioThreadLoop();

		bool m_Initialized { false };
		std::atomic<bool> m_Running { false };
		uint32_t m_SampleRate { kDefaultSampleRate };
		uint32_t m_BufferFrames { kDefaultMixBlockFrames };
		AudioRenderCallback m_Callback;

		std::thread m_AudioThread;

		// Windows multimedia waveOut streaming state
		void* m_WaveOutHandle { nullptr }; // HWAVEOUT
		void* m_WaveEvent { nullptr };     // HANDLE
		void* m_WaveHeaders[4] { nullptr, nullptr, nullptr, nullptr }; // WAVEHDR*
		std::vector<int16_t> m_WavePcmBuffers[4];
		std::vector<float> m_BlockLeft;
		std::vector<float> m_BlockRight;
	};
}
