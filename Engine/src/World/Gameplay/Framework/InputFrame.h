#pragma once

#include "World/Core/Export.h"
#include <cstddef>
#include <cstdint>

namespace World::Gameplay
{
#pragma pack(push, 1)
	struct InputFrame
	{
		uint32_t FrameIndex = 0;
		uint32_t PlayerIndex = 0;
		uint64_t ActionButtons = 0;
		int16_t QuantizedAxes[16] = { 0 };
		int16_t MouseX = 0;
		int16_t MouseY = 0;
		int16_t MouseDeltaX = 0;
		int16_t MouseDeltaY = 0;
		int16_t ScrollX = 0;
		int16_t ScrollY = 0;
		uint32_t CustomPayloadHash = 0;
	};
#pragma pack(pop)

	static_assert(sizeof(InputFrame) == 64, "InputFrame must be exactly 64 bytes POD for deterministic networking & replay");
	static_assert(offsetof(InputFrame, FrameIndex) == 0, "InputFrame offset error: FrameIndex");
	static_assert(offsetof(InputFrame, PlayerIndex) == 4, "InputFrame offset error: PlayerIndex");
	static_assert(offsetof(InputFrame, ActionButtons) == 8, "InputFrame offset error: ActionButtons");
	static_assert(offsetof(InputFrame, QuantizedAxes) == 16, "InputFrame offset error: QuantizedAxes");
	static_assert(offsetof(InputFrame, MouseX) == 48, "InputFrame offset error: MouseX");
	static_assert(offsetof(InputFrame, MouseY) == 50, "InputFrame offset error: MouseY");
	static_assert(offsetof(InputFrame, MouseDeltaX) == 52, "InputFrame offset error: MouseDeltaX");
	static_assert(offsetof(InputFrame, MouseDeltaY) == 54, "InputFrame offset error: MouseDeltaY");
	static_assert(offsetof(InputFrame, ScrollX) == 56, "InputFrame offset error: ScrollX");
	static_assert(offsetof(InputFrame, ScrollY) == 58, "InputFrame offset error: ScrollY");
	static_assert(offsetof(InputFrame, CustomPayloadHash) == 60, "InputFrame offset error: CustomPayloadHash");
}
