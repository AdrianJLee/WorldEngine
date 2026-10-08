#pragma once

#include "World/Core/Export.h"

#ifdef WLD_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace World::Platform
{
	class WLD_API WindowsRawInput
	{
	public:
#ifdef WLD_PLATFORM_WINDOWS
		static bool RegisterDevice(HWND hwnd);
		static bool ProcessMessage(LPARAM lParam, int& outDeltaX, int& outDeltaY);
#endif
		static int GetAccumulatedRawX() { return s_AccumX; }
		static int GetAccumulatedRawY() { return s_AccumY; }
		static void ResetAccumulated() { s_AccumX = 0; s_AccumY = 0; }

	private:
		inline static int s_AccumX = 0;
		inline static int s_AccumY = 0;
	};
}
