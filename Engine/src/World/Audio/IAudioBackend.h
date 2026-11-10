#pragma once

#include "AudioTypes.h"
#include <cstdint>
#include <functional>

namespace World::Audio
{
	using AudioRenderCallback = std::function<void(float* outLeft, float* outRight, uint32_t frameCount)>;

	class WLD_API IAudioBackend
	{
	public:
		virtual ~IAudioBackend() = default;

		virtual bool Initialize(uint32_t sampleRate, uint32_t bufferFrames) = 0;
		virtual void Shutdown() = 0;
		virtual bool IsInitialized() const noexcept = 0;

		virtual void SetRenderCallback(AudioRenderCallback callback) = 0;
		virtual bool Start() = 0;
		virtual void Stop() = 0;

		virtual uint32_t GetSampleRate() const noexcept = 0;
		virtual uint32_t GetBufferFrames() const noexcept = 0;
	};
}
