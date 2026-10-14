#include "AudioStreamer.h"
#include "AudioBuffer.h"
#include <cstring>
#include <algorithm>

namespace World::Audio
{
	AudioStreamer::AudioStreamer() = default;

	AudioStreamer::~AudioStreamer()
	{
		Close();
	}

	AudioStreamer::AudioStreamer(AudioStreamer&& other) noexcept
	{
		std::lock_guard<std::recursive_mutex> lock(other.m_Mutex);
		m_File = std::move(other.m_File);
		m_FilePath = std::move(other.m_FilePath);
		m_SourceType = other.m_SourceType;
		m_SampleRate = other.m_SampleRate;
		m_Channels = other.m_Channels;
		m_TotalFrames = other.m_TotalFrames;
		m_CurrentFrame = other.m_CurrentFrame;
		m_BitsPerSample = other.m_BitsPerSample;
		m_FormatTag = other.m_FormatTag;
		m_DataOffset = other.m_DataOffset;
		m_DataSizeBytes = other.m_DataSizeBytes;
		m_IsOpen = other.m_IsOpen;
		m_IsEof = other.m_IsEof;
		m_MemoryBuffer = std::move(other.m_MemoryBuffer);
		m_Buffers[0] = std::move(other.m_Buffers[0]);
		m_Buffers[1] = std::move(other.m_Buffers[1]);
		m_BufStartFrame[0] = other.m_BufStartFrame[0];
		m_BufStartFrame[1] = other.m_BufStartFrame[1];
		m_BufFrames[0] = other.m_BufFrames[0];
		m_BufFrames[1] = other.m_BufFrames[1];

		other.m_IsOpen = false;
		other.m_IsEof = true;
	}

	AudioStreamer& AudioStreamer::operator=(AudioStreamer&& other) noexcept
	{
		if (this != &other)
		{
			std::scoped_lock lock(m_Mutex, other.m_Mutex);
			Close();

			m_File = std::move(other.m_File);
			m_FilePath = std::move(other.m_FilePath);
			m_SourceType = other.m_SourceType;
			m_SampleRate = other.m_SampleRate;
			m_Channels = other.m_Channels;
			m_TotalFrames = other.m_TotalFrames;
			m_CurrentFrame = other.m_CurrentFrame;
			m_BitsPerSample = other.m_BitsPerSample;
			m_FormatTag = other.m_FormatTag;
			m_DataOffset = other.m_DataOffset;
			m_DataSizeBytes = other.m_DataSizeBytes;
			m_IsOpen = other.m_IsOpen;
			m_IsEof = other.m_IsEof;
			m_MemoryBuffer = std::move(other.m_MemoryBuffer);
			m_Buffers[0] = std::move(other.m_Buffers[0]);
			m_Buffers[1] = std::move(other.m_Buffers[1]);
			m_BufStartFrame[0] = other.m_BufStartFrame[0];
			m_BufStartFrame[1] = other.m_BufStartFrame[1];
			m_BufFrames[0] = other.m_BufFrames[0];
			m_BufFrames[1] = other.m_BufFrames[1];

			other.m_IsOpen = false;
			other.m_IsEof = true;
		}
		return *this;
	}

	void AudioStreamer::ResetBuffers()
	{
		m_BufStartFrame[0] = 0;
		m_BufStartFrame[1] = 0;
		m_BufFrames[0] = 0;
		m_BufFrames[1] = 0;
		m_Buffers[0].clear();
		m_Buffers[1].clear();
	}

	void AudioStreamer::Close()
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		if (m_File.is_open())
		{
			m_File.close();
		}
		m_FilePath.clear();
		m_SourceType = StreamSourceType::None;
		m_IsOpen = false;
		m_IsEof = true;
		m_CurrentFrame = 0;
		m_TotalFrames = 0;
		m_MemoryBuffer.clear();
		ResetBuffers();
	}

	bool AudioStreamer::Open(const std::string& filePath, uint32_t sampleRate, uint32_t channels)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		Close();

		m_File.open(filePath, std::ios::binary);
		if (!m_File.is_open())
		{
			return false;
		}

		m_FilePath = filePath;

		// 1. Check for RIFF WAVE file header
		char header[12];
		m_File.read(header, 12);
		if (m_File.gcount() == 12 &&
		    std::memcmp(header, "RIFF", 4) == 0 &&
		    std::memcmp(header + 8, "WAVE", 4) == 0)
		{
			m_SourceType = StreamSourceType::FileWav;
			m_FormatTag = 1;
			m_Channels = 2;
			m_SampleRate = kDefaultSampleRate;
			m_BitsPerSample = 16;
			m_DataOffset = 0;
			m_DataSizeBytes = 0;

			// Parse WAV chunks
			while (m_File)
			{
				char chunkId[4];
				m_File.read(chunkId, 4);
				if (m_File.gcount() < 4) break;

				uint32_t chunkSize = 0;
				m_File.read(reinterpret_cast<char*>(&chunkSize), 4);
				if (m_File.gcount() < 4) break;

				if (std::memcmp(chunkId, "fmt ", 4) == 0)
				{
					uint16_t wFormatTag = 0;
					uint16_t nChannels = 0;
					uint32_t nSamplesPerSec = 0;
					uint32_t nAvgBytesPerSec = 0;
					uint16_t nBlockAlign = 0;
					uint16_t wBitsPerSample = 0;

					m_File.read(reinterpret_cast<char*>(&wFormatTag), 2);
					m_File.read(reinterpret_cast<char*>(&nChannels), 2);
					m_File.read(reinterpret_cast<char*>(&nSamplesPerSec), 4);
					m_File.read(reinterpret_cast<char*>(&nAvgBytesPerSec), 4);
					m_File.read(reinterpret_cast<char*>(&nBlockAlign), 2);
					m_File.read(reinterpret_cast<char*>(&wBitsPerSample), 2);

					m_FormatTag = wFormatTag;
					m_Channels = nChannels;
					m_SampleRate = nSamplesPerSec;
					m_BitsPerSample = wBitsPerSample;

					if (chunkSize > 16)
					{
						m_File.seekg(chunkSize - 16, std::ios::cur);
					}
				}
				else if (std::memcmp(chunkId, "data", 4) == 0)
				{
					m_DataOffset = static_cast<uint64_t>(m_File.tellg());
					m_DataSizeBytes = chunkSize;
					break;
				}
				else
				{
					m_File.seekg(chunkSize, std::ios::cur);
				}
			}

			if (m_Channels == 0 || m_SampleRate == 0 || m_DataOffset == 0)
			{
				m_File.close();
				return false;
			}

			uint32_t bytesPerFrame = m_Channels * (m_BitsPerSample / 8);
			m_TotalFrames = (bytesPerFrame > 0) ? (m_DataSizeBytes / bytesPerFrame) : 0;
		}
		else
		{
			// Check CookedAudioHeader (magic 0x31434157 "WAC1")
			m_File.seekg(0, std::ios::beg);
			uint32_t magic = 0;
			m_File.read(reinterpret_cast<char*>(&magic), 4);
			if (magic == 0x31434157)
			{
				m_SourceType = StreamSourceType::FileCooked;
				uint32_t version = 0;
				uint32_t sRate = 0;
				uint32_t chans = 0;
				uint32_t fCount = 0;
				uint8_t fmt = 0;
				uint8_t lMode = 0;
				uint16_t flags = 0;
				uint32_t lStart = 0;
				uint32_t lEnd = 0;
				uint32_t dSize = 0;

				m_File.read(reinterpret_cast<char*>(&version), 4);
				m_File.read(reinterpret_cast<char*>(&sRate), 4);
				m_File.read(reinterpret_cast<char*>(&chans), 4);
				m_File.read(reinterpret_cast<char*>(&fCount), 4);
				m_File.read(reinterpret_cast<char*>(&fmt), 1);
				m_File.read(reinterpret_cast<char*>(&lMode), 1);
				m_File.read(reinterpret_cast<char*>(&flags), 2);
				m_File.read(reinterpret_cast<char*>(&lStart), 4);
				m_File.read(reinterpret_cast<char*>(&lEnd), 4);
				m_File.read(reinterpret_cast<char*>(&dSize), 4);

				m_SampleRate = sRate;
				m_Channels = chans;
				m_TotalFrames = fCount;
				m_DataSizeBytes = dSize;
				m_DataOffset = 36; // size of CookedAudioHeader
				m_BitsPerSample = (fmt == 0) ? 16 : 32;
			}
			else
			{
				// Raw PCM fallback
				m_File.seekg(0, std::ios::end);
				uint64_t fileSize = static_cast<uint64_t>(m_File.tellg());
				m_File.seekg(0, std::ios::beg);

				m_SourceType = StreamSourceType::FileRawPcm;
				m_SampleRate = (sampleRate > 0) ? sampleRate : kDefaultSampleRate;
				m_Channels = (channels > 0) ? channels : 2;
				m_BitsPerSample = 32;
				m_DataOffset = 0;
				m_DataSizeBytes = fileSize;
				uint32_t bytesPerFrame = m_Channels * sizeof(float);
				m_TotalFrames = (bytesPerFrame > 0) ? (fileSize / bytesPerFrame) : 0;
			}
		}

		if (m_Channels == 0 || m_SampleRate == 0)
		{
			m_File.close();
			return false;
		}

		m_Buffers[0].assign(kChunkSizeFrames * m_Channels, 0.0f);
		m_Buffers[1].assign(kChunkSizeFrames * m_Channels, 0.0f);
		m_CurrentFrame = 0;
		m_IsEof = (m_TotalFrames == 0);

		FillBuffer(0, 0);
		FillBuffer(1, kChunkSizeFrames);

		m_IsOpen = true;
		return true;
	}

	bool AudioStreamer::OpenMemory(const float* interleavedPcm, uint64_t frameCount, uint32_t sampleRate, uint32_t channels)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		Close();

		if (!interleavedPcm || frameCount == 0 || sampleRate == 0 || channels == 0)
		{
			return false;
		}

		m_SourceType = StreamSourceType::Memory;
		m_SampleRate = sampleRate;
		m_Channels = channels;
		m_TotalFrames = frameCount;
		m_CurrentFrame = 0;
		m_IsEof = false;
		m_BitsPerSample = 32;
		m_MemoryBuffer.assign(interleavedPcm, interleavedPcm + frameCount * channels);

		m_Buffers[0].assign(kChunkSizeFrames * m_Channels, 0.0f);
		m_Buffers[1].assign(kChunkSizeFrames * m_Channels, 0.0f);

		FillBuffer(0, 0);
		FillBuffer(1, kChunkSizeFrames);

		m_IsOpen = true;
		return true;
	}

	bool AudioStreamer::OpenFromBuffer(const AudioBuffer& buffer)
	{
		return OpenMemory(buffer.GetSamples().data(), buffer.GetFrameCount(), buffer.GetSampleRate(), buffer.GetChannels());
	}

	void AudioStreamer::FillBuffer(size_t bufIndex, uint64_t startFrame)
	{
		if (bufIndex >= 2) return;

		if (startFrame >= m_TotalFrames)
		{
			m_BufStartFrame[bufIndex] = startFrame;
			m_BufFrames[bufIndex] = 0;
			std::fill(m_Buffers[bufIndex].begin(), m_Buffers[bufIndex].end(), 0.0f);
			return;
		}

		uint32_t framesToRead = static_cast<uint32_t>(std::min<uint64_t>(kChunkSizeFrames, m_TotalFrames - startFrame));
		m_BufStartFrame[bufIndex] = startFrame;
		m_BufFrames[bufIndex] = framesToRead;

		if (m_SourceType == StreamSourceType::Memory)
		{
			size_t sampleOffset = static_cast<size_t>(startFrame * m_Channels);
			size_t samplesToCopy = static_cast<size_t>(framesToRead * m_Channels);
			std::memcpy(m_Buffers[bufIndex].data(), m_MemoryBuffer.data() + sampleOffset, samplesToCopy * sizeof(float));
		}
		else if (m_SourceType == StreamSourceType::FileWav)
		{
			if (m_BitsPerSample == 16)
			{
				uint64_t byteOffset = m_DataOffset + startFrame * m_Channels * 2;
				m_File.clear();
				m_File.seekg(static_cast<std::streamoff>(byteOffset), std::ios::beg);

				std::vector<int16_t> tempPcm(static_cast<size_t>(framesToRead) * m_Channels);
				m_File.read(reinterpret_cast<char*>(tempPcm.data()), framesToRead * m_Channels * sizeof(int16_t));

				for (size_t i = 0; i < tempPcm.size(); ++i)
				{
					m_Buffers[bufIndex][i] = static_cast<float>(tempPcm[i]) / 32768.0f;
				}
			}
			else
			{
				uint64_t byteOffset = m_DataOffset + startFrame * m_Channels * 4;
				m_File.clear();
				m_File.seekg(static_cast<std::streamoff>(byteOffset), std::ios::beg);
				m_File.read(reinterpret_cast<char*>(m_Buffers[bufIndex].data()), framesToRead * m_Channels * sizeof(float));
			}
		}
		else if (m_SourceType == StreamSourceType::FileCooked || m_SourceType == StreamSourceType::FileRawPcm)
		{
			uint64_t byteOffset = m_DataOffset + startFrame * m_Channels * sizeof(float);
			m_File.clear();
			m_File.seekg(static_cast<std::streamoff>(byteOffset), std::ios::beg);
			m_File.read(reinterpret_cast<char*>(m_Buffers[bufIndex].data()), framesToRead * m_Channels * sizeof(float));
		}

		// Zero padding for trailing space in chunk
		if (framesToRead < kChunkSizeFrames)
		{
			size_t offset = static_cast<size_t>(framesToRead) * m_Channels;
			std::fill(m_Buffers[bufIndex].begin() + offset, m_Buffers[bufIndex].end(), 0.0f);
		}
	}

	uint32_t AudioStreamer::ReadFrames(float* outInterleaved, uint32_t frameCount)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		if (!m_IsOpen || !outInterleaved || frameCount == 0 || m_CurrentFrame >= m_TotalFrames)
		{
			m_IsEof = (m_CurrentFrame >= m_TotalFrames);
			return 0;
		}

		uint32_t totalFramesRead = 0;

		while (totalFramesRead < frameCount && m_CurrentFrame < m_TotalFrames)
		{
			size_t activeBuf = 2;
			for (size_t b = 0; b < 2; ++b)
			{
				if (m_CurrentFrame >= m_BufStartFrame[b] &&
				    m_CurrentFrame < m_BufStartFrame[b] + m_BufFrames[b])
				{
					activeBuf = b;
					break;
				}
			}

			if (activeBuf >= 2)
			{
				// Cache miss: fill buffers starting at current frame
				FillBuffer(0, m_CurrentFrame);
				FillBuffer(1, m_CurrentFrame + kChunkSizeFrames);
				activeBuf = 0;
			}

			size_t offsetInBuf = static_cast<size_t>(m_CurrentFrame - m_BufStartFrame[activeBuf]);
			size_t availableInBuf = m_BufFrames[activeBuf] - offsetInBuf;
			size_t framesNeeded = frameCount - totalFramesRead;
			size_t toCopy = std::min(availableInBuf, framesNeeded);

			if (toCopy > 0)
			{
				const float* src = m_Buffers[activeBuf].data() + offsetInBuf * m_Channels;
				float* dst = outInterleaved + static_cast<size_t>(totalFramesRead) * m_Channels;
				std::memcpy(dst, src, toCopy * m_Channels * sizeof(float));

				m_CurrentFrame += toCopy;
				totalFramesRead += static_cast<uint32_t>(toCopy);
			}

			// Prefetch next chunk if buffer was completely consumed
			if (m_CurrentFrame == m_BufStartFrame[activeBuf] + m_BufFrames[activeBuf])
			{
				size_t otherBuf = 1 - activeBuf;
				uint64_t nextStart = m_BufStartFrame[otherBuf] + kChunkSizeFrames;
				FillBuffer(activeBuf, nextStart);
			}
		}

		if (totalFramesRead < frameCount)
		{
			std::fill_n(outInterleaved + static_cast<size_t>(totalFramesRead) * m_Channels,
			            static_cast<size_t>(frameCount - totalFramesRead) * m_Channels,
			            0.0f);
		}

		m_IsEof = (m_CurrentFrame >= m_TotalFrames);
		return totalFramesRead;
	}

	void AudioStreamer::SeekFrame(uint64_t targetFrame)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		if (!m_IsOpen) return;

		m_CurrentFrame = std::min(targetFrame, m_TotalFrames);
		m_IsEof = (m_CurrentFrame >= m_TotalFrames);

		FillBuffer(0, m_CurrentFrame);
		FillBuffer(1, m_CurrentFrame + kChunkSizeFrames);
	}

	bool AudioStreamer::IsEof() const noexcept
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		return m_IsEof || (m_CurrentFrame >= m_TotalFrames);
	}
}