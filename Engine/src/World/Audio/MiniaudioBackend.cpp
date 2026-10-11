#include "MiniaudioBackend.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <chrono>
#include <algorithm>

namespace World::Audio
{
	constexpr int kNumWaveBuffers = 4;

	MiniaudioBackend::MiniaudioBackend() = default;

	MiniaudioBackend::~MiniaudioBackend()
	{
		Shutdown();
	}

	bool MiniaudioBackend::Initialize(uint32_t sampleRate, uint32_t bufferFrames)
	{
		m_SampleRate = (sampleRate > 0) ? sampleRate : kDefaultSampleRate;
		m_BufferFrames = (bufferFrames > 0) ? bufferFrames : kDefaultMixBlockFrames;
		m_BlockLeft.assign(m_BufferFrames, 0.0f);
		m_BlockRight.assign(m_BufferFrames, 0.0f);
		m_Initialized = true;
		return true;
	}

	void MiniaudioBackend::Shutdown()
	{
		Stop();
		m_Initialized = false;
	}

	bool MiniaudioBackend::Start()
	{
		if (!m_Initialized || m_Running) return false;

		// 1. Create synchronization event for buffer completion
		HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		m_WaveEvent = static_cast<void*>(hEvent);

		// 2. Configure 16-bit stereo PCM format for Windows WaveOut mapper
		WAVEFORMATEX wfx{};
		wfx.wFormatTag = WAVE_FORMAT_PCM;
		wfx.nChannels = 2;
		wfx.nSamplesPerSec = m_SampleRate;
		wfx.wBitsPerSample = 16;
		wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8; // 4 bytes
		wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
		wfx.cbSize = 0;

		HWAVEOUT hWaveOut = nullptr;
		MMRESULT res = waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, reinterpret_cast<DWORD_PTR>(hEvent), 0, CALLBACK_EVENT);
		if (res == MMSYSERR_NOERROR)
		{
			m_WaveOutHandle = static_cast<void*>(hWaveOut);

			// 3. Allocate and prepare circular wave headers
			for (int i = 0; i < kNumWaveBuffers; ++i)
			{
				m_WavePcmBuffers[i].assign(static_cast<size_t>(m_BufferFrames) * 2, 0);

				auto* hdr = new WAVEHDR();
				std::memset(hdr, 0, sizeof(WAVEHDR));
				hdr->lpData = reinterpret_cast<LPSTR>(m_WavePcmBuffers[i].data());
				hdr->dwBufferLength = static_cast<DWORD>(m_WavePcmBuffers[i].size() * sizeof(int16_t));

				waveOutPrepareHeader(hWaveOut, hdr, sizeof(WAVEHDR));
				hdr->dwFlags |= WHDR_DONE; // Mark ready for immediate playback filling
				m_WaveHeaders[i] = static_cast<void*>(hdr);
			}
		}

		m_Running = true;
		m_AudioThread = std::thread(&MiniaudioBackend::AudioThreadLoop, this);
		return true;
	}

	void MiniaudioBackend::Stop()
	{
		if (m_Running)
		{
			m_Running = false;
			if (m_AudioThread.joinable())
			{
				m_AudioThread.join();
			}
		}

		if (m_WaveOutHandle)
		{
			auto hWaveOut = static_cast<HWAVEOUT>(m_WaveOutHandle);
			waveOutReset(hWaveOut);

			for (int i = 0; i < kNumWaveBuffers; ++i)
			{
				if (m_WaveHeaders[i])
				{
					auto* hdr = static_cast<WAVEHDR*>(m_WaveHeaders[i]);
					waveOutUnprepareHeader(hWaveOut, hdr, sizeof(WAVEHDR));
					delete hdr;
					m_WaveHeaders[i] = nullptr;
				}
				m_WavePcmBuffers[i].clear();
			}

			waveOutClose(hWaveOut);
			m_WaveOutHandle = nullptr;
		}

		if (m_WaveEvent)
		{
			CloseHandle(static_cast<HANDLE>(m_WaveEvent));
			m_WaveEvent = nullptr;
		}
	}

	void MiniaudioBackend::AudioThreadLoop()
	{
		double blockDurationSec = static_cast<double>(m_BufferFrames) / static_cast<double>(m_SampleRate);
		auto blockDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double>(blockDurationSec));

		auto nextTick = std::chrono::steady_clock::now();

		while (m_Running)
		{
			if (m_WaveOutHandle)
			{
				auto hWaveOut = static_cast<HWAVEOUT>(m_WaveOutHandle);
				auto hEvent = static_cast<HANDLE>(m_WaveEvent);

				bool wroteAny = false;
				for (int i = 0; i < kNumWaveBuffers; ++i)
				{
					auto* hdr = static_cast<WAVEHDR*>(m_WaveHeaders[i]);
					if (hdr && (hdr->dwFlags & WHDR_DONE))
					{
						// Render new block of float audio from mixer
						if (m_Callback)
						{
							std::fill(m_BlockLeft.begin(), m_BlockLeft.end(), 0.0f);
							std::fill(m_BlockRight.begin(), m_BlockRight.end(), 0.0f);
							m_Callback(m_BlockLeft.data(), m_BlockRight.data(), m_BufferFrames);
						}

						// Convert float [-1.0f, 1.0f] to 16-bit signed PCM
						int16_t* pcm = m_WavePcmBuffers[i].data();
						for (uint32_t f = 0; f < m_BufferFrames; ++f)
						{
							float sL = std::clamp(m_BlockLeft[f] * 32767.0f, -32768.0f, 32767.0f);
							float sR = std::clamp(m_BlockRight[f] * 32767.0f, -32768.0f, 32767.0f);
							pcm[f * 2] = static_cast<int16_t>(sL);
							pcm[f * 2 + 1] = static_cast<int16_t>(sR);
						}

						hdr->dwFlags &= ~WHDR_DONE;
						waveOutWrite(hWaveOut, hdr, sizeof(WAVEHDR));
						wroteAny = true;
					}
				}

				if (!wroteAny)
				{
					WaitForSingleObject(hEvent, 10);
				}
			}
			else
			{
				// Fallback timing mode if hardware sound device is unavailable
				if (m_Callback)
				{
					std::fill(m_BlockLeft.begin(), m_BlockLeft.end(), 0.0f);
					std::fill(m_BlockRight.begin(), m_BlockRight.end(), 0.0f);
					m_Callback(m_BlockLeft.data(), m_BlockRight.data(), m_BufferFrames);
				}

				nextTick += blockDuration;
				auto now = std::chrono::steady_clock::now();
				if (nextTick > now)
				{
					std::this_thread::sleep_for(nextTick - now);
				}
				else
				{
					nextTick = now;
				}
			}
		}
	}
}
