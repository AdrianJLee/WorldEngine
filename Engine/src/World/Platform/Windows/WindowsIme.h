#pragma once

#include "World/Core/Export.h"
#include <string>
#include <vector>

#ifdef WLD_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace World::Platform
{
	struct ImeState
	{
		std::wstring CompositionString;
		int CursorPosition = 0;
		std::vector<std::wstring> Candidates;
		bool Active = false;
	};

	class WLD_API WindowsIme
	{
	public:
#ifdef WLD_PLATFORM_WINDOWS
		static bool HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, ImeState& outState);
#endif
		static const ImeState& GetCurrentState();
	};
}
