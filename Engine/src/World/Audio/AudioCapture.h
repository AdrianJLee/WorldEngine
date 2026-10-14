#pragma once

#include "AudioTypes.h"
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <string>

namespace World::Audio
{
	class WLD_API IAudioCaptureBackend
	{
	public:
		virtual ~IAudioCaptureBackend() = default;

		using CaptureCallback = std::function<void(const float* samples, uint32_t frameCount)>;

		virtual bool Initialize(uint32_t sampleRate, uint32_t channels) = 0;
		virtual bool Start(CaptureCallback callback) = 0;
		virtual void Stop() = 0;
		virtual bool IsCapturing() const noexcept = 0;
		virtual const char* GetBackendName() const noexcept = 0;
	};

	class WLD_API NullAudioCaptureBackend : public IAudioCaptureBackend
	{
	public:
		NullAudioCaptureBackend() = default;
		~NullAudioCaptureBackend() override = default;

		bool Initialize(uint32_t sampleRate, uint32_t channels) override;
		bool Start(CaptureCallback callback) override;
		void Stop() override;
		bool IsCapturing() const noexcept override { return m_Capturing; }
		const char* GetBackendName() const noexcept override { return "NullCapture"; }

		// Test and simulation helper: simulate incoming microphone PCM frames
		void SimulateMicInput(const float* samples, uint32_t frameCount);

	private:
		bool m_Capturing { false };
		uint32_t m_SampleRate { 48000 };
		uint32_t m_Channels { 1 };
		CaptureCallback m_Callback;
	};

	class WLD_API AudioCaptureDevice
	{
	public:
		AudioCaptureDevice();
		~AudioCaptureDevice();

		bool Initialize(
			std::unique_ptr<IAudioCaptureBackend> backend = nullptr,
			uint32_t sampleRate = kDefaultSampleRate,
			uint32_t channels = 1,
			size_t ringBufferCapacityFrames = 48000);

		void Shutdown();

		bool Start();
		void Stop();
		bool IsCapturing() const noexcept;

		// Push samples into ring buffer (called by capture callback or mic stream)
		void PushSamples(const float* samples, uint32_t frameCount);

		// Pop available samples from ring buffer into destination buffer
		// Returns count of frames actually read
		uint32_t PopSamples(float* outSamples, uint32_t maxFrames);

		uint32_t GetAvailableFrames() const noexcept;
		uint32_t GetBufferCapacity() const noexcept { return static_cast<uint32_t>(m_Capacity); }
		void ClearBuffer() noexcept;

		// RMS loudness level of recent input (0.0 to 1.0)
		float GetInputLevelRms() const noexcept { return m_LastInputRms.load(std::memory_order_relaxed); }
		float GetInputLevelDb() const noexcept { return LinearToDb(GetInputLevelRms()); }

		uint32_t GetSampleRate() const noexcept { return m_SampleRate; }
		uint32_t GetChannels() const noexcept { return m_Channels; }
		const IAudioCaptureBackend* GetBackend() const noexcept { return m_Backend.get(); }

	private:
		std::unique_ptr<IAudioCaptureBackend> m_Backend;
		uint32_t m_SampleRate { kDefaultSampleRate };
		uint32_t m_Channels { 1 };
		size_t m_Capacity { 48000 };

		std::vector<float> m_InputRingBuffer;
		std::atomic<size_t> m_WriteIndex { 0 };
		std::atomic<size_t> m_ReadIndex { 0 };
		std::atomic<float> m_LastInputRms { 0.0f };
		mutable std::mutex m_BufferMutex;
	};
}
