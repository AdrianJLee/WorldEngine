#include "AudioCooker.h"
#include <fstream>
#include <cstring>

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
		size_t rawPayloadBytes = sampleCount * sizeof(float);

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
		hdr.dataSizeBytes = static_cast<uint32_t>(rawPayloadBytes);

		std::vector<uint8_t> result(sizeof(CookedAudioHeader) + rawPayloadBytes);
		std::memcpy(result.data(), &hdr, sizeof(CookedAudioHeader));
		if (rawPayloadBytes > 0)
		{
			std::memcpy(result.data() + sizeof(CookedAudioHeader), processed.GetSamples().data(), rawPayloadBytes);
		}

		return result;
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

		size_t expectedBytes = static_cast<size_t>(hdr.frameCount) * hdr.channels * sizeof(float);
		if (size >= sizeof(CookedAudioHeader) + expectedBytes && expectedBytes > 0)
		{
			std::memcpy(outBuffer.GetSamples().data(), data + sizeof(CookedAudioHeader), expectedBytes);
		}

		return true;
	}

	bool AudioCooker::CookFile(const std::string& sourceWavPath, const std::string& outputCookedPath, const AudioCookSettings& settings)
	{
		// Test sine wave synthetic fallback if WAV loader stubbed
		AudioBuffer buf = AudioBuffer::GenerateSineWave(440.0f, 1.0f, 0.5f);
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
