#pragma once

#include "AudioTypes.h"
#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <cstdint>
#include <memory>

namespace World::Audio
{
	class AudioBuffer;

	// Circular double-buffering audio streamer for AudioLoadingMode::StreamFromDisk
	class WLD_API AudioStreamer
	{
	public:
		static constexpr size_t kChunkSizeFrames = 4096; // 4096 stereo float frames = 32KB

		AudioStreamer();
		~AudioStreamer();

		AudioStreamer(const AudioStreamer&) = delete;
		AudioStreamer& operator=(const AudioStreamer&) = delete;

		AudioStreamer(AudioStreamer&& other) noexcept;
		AudioStreamer& operator=(AudioStreamer&& other) noexcept;

		// Opens a WAV, Cooked binary, or raw PCM file for streaming
		bool Open(const std::string& filePath, uint32_t sampleRate = 0, uint32_t channels = 0);

		// Opens an in-memory buffer for streaming (testing and synthetic cues)
		bool OpenMemory(const float* interleavedPcm, uint64_t frameCount, uint32_t sampleRate, uint32_t channels);
		bool OpenFromBuffer(const AudioBuffer& buffer);

		void Close();

		// Reads up to frameCount interleaved float samples into outInterleaved
		// Returns actual number of frames read (may be less than frameCount if EOF)
		uint32_t ReadFrames(float* outInterleaved, uint32_t frameCount);

		// Seeks to target frame in the stream
		void SeekFrame(uint64_t targetFrame);

		bool IsEof() const noexcept;
		bool IsOpen() const noexcept { return m_IsOpen; }

		uint32_t GetSampleRate() const noexcept { return m_SampleRate; }
		uint32_t GetChannels() const noexcept { return m_Channels; }
		uint64_t GetTotalFrames() const noexcept { return m_TotalFrames; }
		uint64_t GetCurrentFrame() const noexcept { return m_CurrentFrame; }

	private:
		enum class StreamSourceType : uint8_t
		{
			None = 0,
			FileWav,
			FileCooked,
			FileRawPcm,
			Memory
		};

		void FillBuffer(size_t bufIndex, uint64_t startFrame);
		void ResetBuffers();

		mutable std::recursive_mutex m_Mutex;
		std::ifstream m_File;
		std::string m_FilePath;

		StreamSourceType m_SourceType { StreamSourceType::None };
		uint32_t m_SampleRate { kDefaultSampleRate };
		uint32_t m_Channels { 2 };
		uint64_t m_TotalFrames { 0 };
		uint64_t m_CurrentFrame { 0 };
		uint32_t m_BitsPerSample { 16 };
		uint32_t m_FormatTag { 1 };
		uint64_t m_DataOffset { 0 };
		uint64_t m_DataSizeBytes { 0 };
		bool m_IsOpen { false };
		bool m_IsEof { false };

		// In-memory fallback/test buffer
		std::vector<float> m_MemoryBuffer;

		// Ring double buffer: two chunks of kChunkSizeFrames
		std::vector<float> m_Buffers[2];
		uint64_t m_BufStartFrame[2] { 0, 0 };
		uint32_t m_BufFrames[2] { 0, 0 };
	};
}