#pragma once

#include "AudioTypes.h"
#include "World/Scene/Components.h"
#include <cstdint>
#include <type_traits>

namespace World
{
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
