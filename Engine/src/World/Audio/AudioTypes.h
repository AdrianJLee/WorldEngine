#pragma once

#include "World/Core/Core.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <cmath>
#include <string>
#include <string_view>
#include <algorithm>

namespace World::Audio
{
	using SoundEventHandle = uint64_t;
	using VoiceHandle = uint32_t;
	using BusId = uint32_t;

	constexpr VoiceHandle kInvalidVoiceHandle = 0;
	constexpr SoundEventHandle kInvalidEventHandle = 0;
	constexpr BusId kInvalidBusId = 0xFFFFFFFFu;
	constexpr BusId kBusMaster = 0;
	constexpr BusId kBusMusic = 1;
	constexpr BusId kBusSfx = 2;
	constexpr BusId kBusVoice = 3;
	constexpr BusId kBusUi = 4;
	constexpr BusId kBusAmbience = 5;

	constexpr uint32_t kMaxPhysicalVoices = 64;
	constexpr uint32_t kMaxVirtualVoices = 1024;
	constexpr uint32_t kDefaultSampleRate = 48000;
	constexpr uint32_t kDefaultMixBlockFrames = 256; // ~5.33ms at 48kHz

	enum class AudioFormat : uint8_t
	{
		Pcm16 = 0,
		PcmFloat32 = 1,
		Vorbis = 2,
		Adpcm = 3
	};

	enum class SpeakerMode : uint8_t
	{
		Mono = 1,
		Stereo = 2,
		Surround51 = 6
	};

	enum class SpatialMode : uint8_t
	{
		None = 0,
		Spatial3D = 1,
		Spatial2D = 2
	};

	enum class AttenuationModel : uint8_t
	{
		Linear = 0,
		Logarithmic = 1,
		InverseSquare = 2,
		CustomCurve = 3
	};

	enum class StealPolicy : uint8_t
	{
		StealOldest = 0,
		StealQuiet = 1,
		StealLowestPriority = 2,
		StealReject = 3
	};

	enum class AudioLoadingMode : uint8_t
	{
		Resident = 0,        // Fully decoded/buffered in memory
		StreamFromDisk = 1,  // Streamed via circular IO buffer
		DecompressOnLoad = 2 // Compressed in RAM, uncompressed into cache
	};

	enum class VoiceState : uint8_t
	{
		Free = 0,
		Playing = 1,
		Paused = 2,
		Virtual = 3,
		Stopping = 4
	};

	enum class AudioCommandType : uint8_t
	{
		None = 0,
		PlayVoice,
		StopVoice,
		PauseVoice,
		ResumeVoice,
		SetVoiceVolume,
		SetVoicePitch,
		SetVoiceSpatialPosition,
		SetVoiceSpatial2DPosition,
		SetVoiceOcclusion,
		SetBusVolume,
		SetBusMute,
		SetBusSolo,
		TriggerSnapshot,
		ClearSnapshot,
		StopAll
	};

	struct AudioCommand
	{
		AudioCommandType type { AudioCommandType::None };
		uint8_t flags { 0 };
		uint8_t priority { 128 };
		uint8_t padding { 0 };
		VoiceHandle voiceHandle { kInvalidVoiceHandle };
		BusId busId { kInvalidBusId };
		float floatParam0 { 0.0f }; // volume, pitch, transition duration, etc.
		float floatParam1 { 0.0f };
		float floatParam2 { 0.0f };
		glm::vec3 vecParam { 0.0f, 0.0f, 0.0f };
		uint64_t assetId { 0 };
	};

	inline float DbToLinear(float db) noexcept
	{
		if (db <= -96.0f) return 0.0f;
		return std::pow(10.0f, db * 0.05f);
	}

	inline float LinearToDb(float linear) noexcept
	{
		if (linear <= 0.00001f) return -96.0f;
		return 20.0f * std::log10(linear);
	}

	inline float PitchSemitonesToRatio(float semitones) noexcept
	{
		return std::pow(2.0f, semitones / 12.0f);
	}

	struct StereoMeter
	{
		float leftPeakDb { -96.0f };
		float rightPeakDb { -96.0f };
		float leftRmsDb { -96.0f };
		float rightRmsDb { -96.0f };
	};
}
