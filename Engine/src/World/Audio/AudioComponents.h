#pragma once

#include "AudioTypes.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <type_traits>

namespace World
{
	struct AudioSourceComponent
	{
		uint64_t SoundAssetId { 0 };       // 8B: GUID/Hash for .wsound asset
		uint32_t BusOverride { 0xFFFFFFFFu }; // 4B: Optional bus override
		float Volume { 1.0f };             // 4B: Volume [0.0f, 4.0f]
		float Pitch { 1.0f };              // 4B: Pitch ratio [0.1f, 4.0f]
		float MinDistance { 1.0f };        // 4B: 3D Min distance
		float MaxDistance { 25.0f };       // 4B: 3D Max distance
		float InnerAngle { 360.0f };       // 4B: Directional cone inner angle
		float OuterAngle { 360.0f };       // 4B: Directional cone outer angle
		float OuterGain { 1.0f };          // 4B: Directional cone outer gain
		uint8_t Flags { 0x05 };            // 1B: Bit 0: AutoPlay, Bit 1: Looping, Bit 2: Spatial3D
		uint8_t Priority { 128 };          // 1B: Stealing priority 0-255
		uint8_t OcclusionFactor { 0 };     // 1B: Dynamic Jolt raycast occlusion (0~255 maps to 0.0~1.0)
		uint8_t Padding[17] { 0 };         // 17B: Pad to 64 bytes

		bool IsAutoPlay() const noexcept { return (Flags & 0x01) != 0; }
		bool IsLooping() const noexcept { return (Flags & 0x02) != 0; }
		bool IsSpatial3D() const noexcept { return (Flags & 0x04) != 0; }

		void SetAutoPlay(bool v) noexcept { if (v) Flags |= 0x01; else Flags &= ~0x01; }
		void SetLooping(bool v) noexcept { if (v) Flags |= 0x02; else Flags &= ~0x02; }
		void SetSpatial3D(bool v) noexcept { if (v) Flags |= 0x04; else Flags &= ~0x04; }
	};
	static_assert(sizeof(AudioSourceComponent) == 64, "AudioSourceComponent must be exactly 64B cache line size");
	static_assert(std::is_trivially_copyable_v<AudioSourceComponent>, "AudioSourceComponent must be trivially copyable");

	struct AudioListenerComponent
	{
		uint8_t ListenerIndex { 0 };       // 1B: Camera index (0 = main)
		uint8_t Active { 1 };              // 1B: Whether active
		uint8_t Priority { 128 };          // 1B: Priority
		uint8_t Padding0 { 0 };            // 1B
		float VolumeScale { 1.0f };        // 4B: Global master volume scalar
		uint8_t Reserved[24] { 0 };        // 24B
	};
	static_assert(sizeof(AudioListenerComponent) == 32, "AudioListenerComponent must be 32B (<=64B)");
	static_assert(std::is_trivially_copyable_v<AudioListenerComponent>, "AudioListenerComponent must be trivially copyable");

	struct AudioZoneComponent
	{
		uint32_t SnapshotId { 0 };         // 4B: Snapshot to trigger
		float ReverbSendLevel { 0.2f };    // 4B: Wet reverb send
		float HalfExtents[3] { 5.0f, 5.0f, 5.0f }; // 12B: Bounding box
		uint8_t Active { 1 };              // 1B
		uint8_t Priority { 128 };          // 1B
		uint8_t Reserved[10] { 0 };        // 10B
	};
	static_assert(sizeof(AudioZoneComponent) == 32, "AudioZoneComponent must be 32B (<=64B)");
	static_assert(std::is_trivially_copyable_v<AudioZoneComponent>, "AudioZoneComponent must be trivially copyable");
}
