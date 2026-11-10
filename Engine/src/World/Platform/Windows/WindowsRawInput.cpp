#include "wldpch.h"
#include "World/Platform/Windows/WindowsRawInput.h"

namespace World::Platform
{
#ifdef WLD_PLATFORM_WINDOWS
	bool WindowsRawInput::RegisterDevice(HWND hwnd)
	{
		RAWINPUTDEVICE rid;
		rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
		rid.usUsage = 0x02;     // HID_USAGE_GENERIC_MOUSE
		rid.dwFlags = RIDEV_INPUTSINK;
		rid.hwndTarget = hwnd;
		return RegisterRawInputDevices(&rid, 1, sizeof(RAWINPUTDEVICE)) == TRUE;
	}

	bool WindowsRawInput::ProcessMessage(LPARAM lParam, int& outDeltaX, int& outDeltaY)
	{
		UINT dwSize = sizeof(RAWINPUT);
		RAWINPUT raw;
		if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &dwSize, sizeof(RAWINPUTHEADER)) != UINT(-1))
		{
			if (raw.header.dwType == RIM_TYPEMOUSE)
			{
				outDeltaX = raw.data.mouse.lLastX;
				outDeltaY = raw.data.mouse.lLastY;
				s_AccumX += outDeltaX;
				s_AccumY += outDeltaY;
				return true;
			}
		}
		outDeltaX = 0;
		outDeltaY = 0;
		return false;
	}
#endif
}
