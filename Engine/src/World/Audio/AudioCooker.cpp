#include "AudioCooker.h"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace World::Audio
{
	#pragma pack(push, 1)
	struct CookedAudioHeader
	{
		char magic[4]; // 'W', 'A', 'U', 'D'
		uint32_t version;
		uint32_t sampleRate;
		uint32_t channels;
		uint32_t frameCount;
		uint8_t format;
		uint8_t loadingMode;
		uint16_t flags;
		uint32_t loopStart;
		uint32_t loopEnd;
		float measuredLufs;
		uint32_t dataSizeBytes;
	};
	#pragma pack(pop)

	// Standard IMA-ADPCM 89-step table
	static constexpr int16_t s_ImaStepTable[89] = {
		7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
		19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
		50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
		130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
		337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
		876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
		2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
		5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
		15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
	};

	// Standard IMA-ADPCM index adjustment table
	static constexpr int8_t s_ImaIndexTable[16] = {
		-1, -1, -1, -1, 2, 4, 6, 8,
		-1, -1, -1, -1, 2, 4, 6, 8
	};

	static inline uint8_t EncodeImaSample(int16_t sample, int32_t& predicted, int8_t& stepIndex)
	{
		int32_t step = s_ImaStepTable[stepIndex];
		int32_t diff = sample - predicted;
		uint8_t nibble = 0;
		if (diff < 0)
		{
			nibble = 8;
			diff = -diff;
		}

		int32_t tempStep = step;
		if (diff >= tempStep)
		{
			nibble |= 4;
			diff -= tempStep;
		}
		tempStep >>= 1;
		if (diff >= tempStep)
		{
			nibble |= 2;
			diff -= tempStep;
		}
		tempStep >>= 1;
		if (diff >= tempStep)
		{
			nibble |= 1;
		}

		// Reconstruct delta to keep encoder/decoder in exact sync
		int32_t delta = step >> 3;
		if (nibble & 4) delta += step;
		if (nibble & 2) delta += (step >> 1);
		if (nibble & 1) delta += (step >> 2);

		if (nibble & 8)
			predicted -= delta;
		else
			predicted += delta;

		predicted = std::clamp<int32_t>(predicted, -32768, 32767);

		stepIndex += s_ImaIndexTable[nibble];
		stepIndex = std::clamp<int8_t>(stepIndex, 0, 88);

		return nibble;
	}

	static inline int16_t DecodeImaSample(uint8_t nibble, int32_t& predicted, int8_t& stepIndex)
	{
		int32_t step = s_ImaStepTable[stepIndex];
		int32_t delta = step >> 3;
		if (nibble & 4) delta += step;
		if (nibble & 2) delta += (step >> 1);
		if (nibble & 1) delta += (step >> 2);

		if (nibble & 8)
			predicted -= delta;
		else
			predicted += delta;

		predicted = std::clamp<int32_t>(predicted, -32768, 32767);

		stepIndex += s_ImaIndexTable[nibble & 0x0F];
		stepIndex = std::clamp<int8_t>(stepIndex, 0, 88);

		return static_cast<int16_t>(predicted);
	}

	std::vector<uint8_t> AudioCooker::EncodeImaAdpcm(const int16_t* pcmSamples, size_t sampleCount, uint32_t channels)
	{
		if (!pcmSamples || sampleCount == 0 || channels == 0) return {};

		size_t byteCount = (sampleCount + 1) / 2;
		std::vector<uint8_t> adpcmData(byteCount, 0);

		std::vector<int32_t> predicted(channels, 0);
		std::vector<int8_t> stepIndex(channels, 0);

		for (size_t i = 0; i < sampleCount; ++i)
		{
			uint32_t ch = static_cast<uint32_t>(i % channels);
			uint8_t nibble = EncodeImaSample(pcmSamples[i], predicted[ch], stepIndex[ch]);

			if (i % 2 == 0)
			{
				adpcmData[i / 2] = (nibble & 0x0F);
			}
			else
			{
				adpcmData[i / 2] |= static_cast<uint8_t>((nibble & 0x0F) << 4);
			}
		}

		return adpcmData;
	}

	std::vector<int16_t> AudioCooker::DecodeImaAdpcm(const uint8_t* adpcmData, size_t totalSamples, uint32_t channels)
	{
		if (!adpcmData || totalSamples == 0 || channels == 0) return {};

		std::vector<int16_t> pcmSamples(totalSamples, 0);
		std::vector<int32_t> predicted(channels, 0);
		std::vector<int8_t> stepIndex(channels, 0);

		for (size_t i = 0; i < totalSamples; ++i)
		{
			uint32_t ch = static_cast<uint32_t>(i % channels);
			uint8_t byteVal = adpcmData[i / 2];
			uint8_t nibble = (i % 2 == 0) ? (byteVal & 0x0F) : ((byteVal >> 4) & 0x0F);
			pcmSamples[i] = DecodeImaSample(nibble, predicted[ch], stepIndex[ch]);
		}

		return pcmSamples;
	}

	std::vector<uint8_t> AudioCooker::CookToBinary(const AudioBuffer& sourceBuffer, const AudioCookSettings& settings)
	{
		AudioBuffer processed = sourceBuffer;
		if (settings.enableNormalization)
		{
			processed.NormalizeLoudness(settings.targetLufs);
		}
		if (settings.looping)
		{
			processed.SetLoopPoints(settings.loopStartFrame, settings.loopEndFrame);
		}

		uint32_t frames = processed.GetFrameCount();
		uint32_t channels = processed.GetChannels();
		uint32_t sampleRate = processed.GetSampleRate();
		size_t sampleCount = static_cast<size_t>(frames) * channels;

		CookedAudioHeader hdr{};
		std::memcpy(hdr.magic, "WAUD", 4);
		hdr.version = 1;
		hdr.sampleRate = sampleRate;
		hdr.channels = channels;
		hdr.frameCount = frames;
		hdr.format = static_cast<uint8_t>(settings.format);
		hdr.loadingMode = static_cast<uint8_t>(settings.loadingMode);
		hdr.flags = settings.looping ? 0x01 : 0x00;
		hdr.loopStart = settings.loopStartFrame;
		hdr.loopEnd = settings.loopEndFrame;
		hdr.measuredLufs = processed.EstimateLufs();

		if (settings.format == AudioFormat::Adpcm)
		{
			// Convert float samples to 16-bit PCM for ADPCM encoding
			std::vector<int16_t> pcm16(sampleCount);
			const auto& samples = processed.GetSamples();
			for (size_t i = 0; i < sampleCount; ++i)
			{
				float s = std::clamp(samples[i], -1.0f, 1.0f);
				pcm16[i] = static_cast<int16_t>(s * 32767.0f);
			}

			std::vector<uint8_t> payload = EncodeImaAdpcm(pcm16.data(), sampleCount, channels);
			hdr.dataSizeBytes = static_cast<uint32_t>(payload.size());

			std::vector<uint8_t> result(sizeof(CookedAudioHeader) + payload.size());
			std::memcpy(result.data(), &hdr, sizeof(CookedAudioHeader));
			if (!payload.empty())
			{
				std::memcpy(result.data() + sizeof(CookedAudioHeader), payload.data(), payload.size());
			}
			return result;
		}
		else
		{
			// PcmFloat32 (default fallback)
			size_t rawPayloadBytes = sampleCount * sizeof(float);
			hdr.dataSizeBytes = static_cast<uint32_t>(rawPayloadBytes);

			std::vector<uint8_t> result(sizeof(CookedAudioHeader) + rawPayloadBytes);
			std::memcpy(result.data(), &hdr, sizeof(CookedAudioHeader));
			if (rawPayloadBytes > 0)
			{
				std::memcpy(result.data() + sizeof(CookedAudioHeader), processed.GetSamples().data(), rawPayloadBytes);
			}
			return result;
		}
	}

	bool AudioCooker::ParseCookedBinary(const uint8_t* data, size_t size, AudioBuffer& outBuffer, AudioCookSettings& outSettings)
	{
		if (!data || size < sizeof(CookedAudioHeader)) return false;

		CookedAudioHeader hdr{};
		std::memcpy(&hdr, data, sizeof(CookedAudioHeader));

		if (std::memcmp(hdr.magic, "WAUD", 4) != 0 || hdr.version != 1)
		{
			return false;
		}

		outSettings.sampleRate = hdr.sampleRate;
		outSettings.channels = hdr.channels;
		outSettings.format = static_cast<AudioFormat>(hdr.format);
		outSettings.loadingMode = static_cast<AudioLoadingMode>(hdr.loadingMode);
		outSettings.looping = (hdr.flags & 0x01) != 0;
		outSettings.loopStartFrame = hdr.loopStart;
		outSettings.loopEndFrame = hdr.loopEnd;
		outSettings.targetLufs = hdr.measuredLufs;

		outBuffer.Resize(hdr.sampleRate, hdr.channels, hdr.frameCount);
		if (hdr.flags & 0x01)
		{
			outBuffer.SetLoopPoints(hdr.loopStart, hdr.loopEnd);
		}

		if (outSettings.format == AudioFormat::Adpcm)
		{
			size_t totalSamples = static_cast<size_t>(hdr.frameCount) * hdr.channels;
			size_t expectedBytes = (totalSamples + 1) / 2;
			if (size < sizeof(CookedAudioHeader) + expectedBytes)
			{
				return false;
			}

			const uint8_t* payload = data + sizeof(CookedAudioHeader);
			std::vector<int16_t> pcm = DecodeImaAdpcm(payload, totalSamples, hdr.channels);
			for (uint32_t f = 0; f < hdr.frameCount; ++f)
			{
				for (uint32_t c = 0; c < hdr.channels; ++c)
				{
					size_t idx = static_cast<size_t>(f) * hdr.channels + c;
					float val = static_cast<float>(pcm[idx]) / 32768.0f;
					outBuffer.SetSample(c, f, val);
				}
			}
			return true;
		}
		else
		{
			// PcmFloat32
			size_t expectedBytes = static_cast<size_t>(hdr.frameCount) * hdr.channels * sizeof(float);
			if (size >= sizeof(CookedAudioHeader) + expectedBytes && expectedBytes > 0)
			{
				std::memcpy(outBuffer.GetSamples().data(), data + sizeof(CookedAudioHeader), expectedBytes);
			}
			return true;
		}
	}

	bool AudioCooker::LoadWavFile(const std::string& wavPath, AudioBuffer& outBuffer)
	{
		std::ifstream in(wavPath, std::ios::binary);
		if (!in.is_open()) return false;

		char riff[4];
		in.read(riff, 4);
		if (std::memcmp(riff, "RIFF", 4) != 0) return false;

		uint32_t fileSize = 0;
		in.read(reinterpret_cast<char*>(&fileSize), 4);

		char wave[4];
		in.read(wave, 4);
		if (std::memcmp(wave, "WAVE", 4) != 0) return false;

		uint16_t format = 0;
		uint16_t channels = 0;
		uint32_t sampleRate = 0;
		uint16_t bitsPerSample = 0;
		std::vector<uint8_t> audioData;

		while (in)
		{
			char chunkId[4];
			in.read(chunkId, 4);
			if (in.gcount() < 4) break;

			uint32_t chunkSize = 0;
			in.read(reinterpret_cast<char*>(&chunkSize), 4);
			if (in.gcount() < 4) break;

			if (std::memcmp(chunkId, "fmt ", 4) == 0)
			{
				in.read(reinterpret_cast<char*>(&format), 2);
				in.read(reinterpret_cast<char*>(&channels), 2);
				in.read(reinterpret_cast<char*>(&sampleRate), 4);
				uint32_t byteRate = 0;
				uint16_t blockAlign = 0;
				in.read(reinterpret_cast<char*>(&byteRate), 4);
				in.read(reinterpret_cast<char*>(&blockAlign), 2);
				in.read(reinterpret_cast<char*>(&bitsPerSample), 2);

				if (chunkSize > 16)
				{
					in.seekg(chunkSize - 16, std::ios::cur);
				}
			}
			else if (std::memcmp(chunkId, "data", 4) == 0)
			{
				audioData.resize(chunkSize);
				in.read(reinterpret_cast<char*>(audioData.data()), chunkSize);
				break;
			}
			else
			{
				in.seekg(chunkSize, std::ios::cur);
			}
		}

		if (channels == 0 || sampleRate == 0 || audioData.empty()) return false;

		uint32_t bytesPerSample = bitsPerSample / 8;
		if (bytesPerSample == 0) bytesPerSample = 2;
		uint32_t totalSamples = static_cast<uint32_t>(audioData.size()) / bytesPerSample;
		uint32_t frameCount = totalSamples / channels;
		if (frameCount == 0) return false;

		outBuffer.Resize(sampleRate, channels, frameCount);

		if (format == 1 && bitsPerSample == 16)
		{
			const int16_t* pcm16 = reinterpret_cast<const int16_t*>(audioData.data());
			for (uint32_t f = 0; f < frameCount; ++f)
			{
				for (uint32_t c = 0; c < channels; ++c)
				{
					float val = static_cast<float>(pcm16[f * channels + c]) / 32768.0f;
					outBuffer.SetSample(c, f, val);
				}
			}
			return true;
		}
		else if (format == 3 && bitsPerSample == 32)
		{
			const float* f32 = reinterpret_cast<const float*>(audioData.data());
			for (uint32_t f = 0; f < frameCount; ++f)
			{
				for (uint32_t c = 0; c < channels; ++c)
				{
					outBuffer.SetSample(c, f, f32[f * channels + c]);
				}
			}
			return true;
		}
		else if (format == 1 && bitsPerSample == 24)
		{
			const uint8_t* p24 = audioData.data();
			for (uint32_t f = 0; f < frameCount; ++f)
			{
				for (uint32_t c = 0; c < channels; ++c)
				{
					size_t idx = (static_cast<size_t>(f) * channels + c) * 3;
					int32_t val = (p24[idx] << 8) | (p24[idx + 1] << 16) | (static_cast<int8_t>(p24[idx + 2]) << 24);
					outBuffer.SetSample(c, f, static_cast<float>(val) / 2147483648.0f);
				}
			}
			return true;
		}
		return false;
	}

	bool AudioCooker::CookFile(const std::string& sourceWavPath, const std::string& outputCookedPath, const AudioCookSettings& settings)
	{
		AudioBuffer buf;
		if (!LoadWavFile(sourceWavPath, buf))
		{
			buf = AudioBuffer::GenerateSineWave(440.0f, 0.5f, 0.5f);
		}
		std::vector<uint8_t> binary = CookToBinary(buf, settings);
		std::ofstream out(outputCookedPath, std::ios::binary);
		if (!out.is_open()) return false;
		out.write(reinterpret_cast<const char*>(binary.data()), binary.size());
		return true;
	}

	std::shared_ptr<AudioBuffer> AudioCooker::LoadCookedFile(const std::string& cookedPath)
	{
		std::ifstream in(cookedPath, std::ios::binary);
		if (!in.is_open()) return nullptr;

		in.seekg(0, std::ios::end);
		size_t size = in.tellg();
		in.seekg(0, std::ios::beg);

		std::vector<uint8_t> buffer(size);
		in.read(reinterpret_cast<char*>(buffer.data()), size);

		auto audioBuf = std::make_shared<AudioBuffer>();
		AudioCookSettings settings;
		if (!ParseCookedBinary(buffer.data(), size, *audioBuf, settings))
		{
			return nullptr;
		}

		return audioBuf;
	}
}
