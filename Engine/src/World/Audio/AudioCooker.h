#pragma once

#include "AudioTypes.h"
#include "AudioBuffer.h"
#include <string>
#include <vector>
#include <memory>

namespace World::Audio
{
	struct AudioCookSettings
	{
		std::string sourcePath;
		AudioFormat format { AudioFormat::Vorbis };
		AudioLoadingMode loadingMode { AudioLoadingMode::Resident };
		uint32_t sampleRate { kDefaultSampleRate };
		uint32_t channels { 2 };
		float targetLufs { -16.0f };
		bool enableNormalization { true };
		bool looping { false };
		uint32_t loopStartFrame { 0 };
		uint32_t loopEndFrame { 0 };
	};

	class WLD_API AudioCooker
	{
	public:
		static std::vector<uint8_t> CookToBinary(const AudioBuffer& sourceBuffer, const AudioCookSettings& settings);
		static bool ParseCookedBinary(const uint8_t* data, size_t size, AudioBuffer& outBuffer, AudioCookSettings& outSettings);

		static bool LoadWavFile(const std::string& wavPath, AudioBuffer& outBuffer);
		static bool CookFile(const std::string& sourceWavPath, const std::string& outputCookedPath, const AudioCookSettings& settings);
		static std::shared_ptr<AudioBuffer> LoadCookedFile(const std::string& cookedPath);

		// IMA-ADPCM 4:1 compression codecs
		static std::vector<uint8_t> EncodeImaAdpcm(const int16_t* pcmSamples, size_t sampleCount, uint32_t channels);
		static std::vector<int16_t> DecodeImaAdpcm(const uint8_t* adpcmData, size_t totalSamples, uint32_t channels);
	};
}
