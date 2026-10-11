#pragma once

#include "IAudioBackend.h"
#include "AudioBuffer.h"
#include <vector>

namespace World::Audio
{
	class WLD_API AudioNullBackend final : public IAudioBackend
	{
	public:
		AudioNullBackend() = default;
		~AudioNullBackend() override;

		bool Initialize(uint32_t sampleRate, uint32_t bufferFrames) override;
		void Shutdown() override;
		bool IsInitialized() const noexcept override { return m_Initialized; }

		void SetRenderCallback(AudioRenderCallback callback) override { m_Callback = std::move(callback); }
		bool Start() override;
		void Stop() override;

		uint32_t GetSampleRate() const noexcept override { return m_SampleRate; }
		uint32_t GetBufferFrames() const noexcept override { return m_BufferFrames; }

		// Offline step simulation for headless testing
		void StepMix(uint32_t frameCount);
		void StepSeconds(float seconds);

		const std::vector<float>& GetCapturedLeft() const noexcept { return m_CapturedLeft; }
		const std::vector<float>& GetCapturedRight() const noexcept { return m_CapturedRight; }
		void ClearCapture() noexcept;

		AudioBuffer ExportCapturedBuffer() const;

	private:
		bool m_Initialized { false };
		bool m_Running { false };
		uint32_t m_SampleRate { kDefaultSampleRate };
		uint32_t m_BufferFrames { kDefaultMixBlockFrames };
		AudioRenderCallback m_Callback;

		std::vector<float> m_CapturedLeft;
		std::vector<float> m_CapturedRight;
		std::vector<float> m_ScratchLeft;
		std::vector<float> m_ScratchRight;
	};
}
