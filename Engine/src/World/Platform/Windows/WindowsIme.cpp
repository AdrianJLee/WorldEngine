#include "wldpch.h"
#include "World/Platform/Windows/WindowsIme.h"

#ifdef WLD_PLATFORM_WINDOWS
#include <imm.h>
#endif

namespace World::Platform
{
	namespace
	{
		ImeState s_CurrentState;
	}

	const ImeState& WindowsIme::GetCurrentState()
	{
		return s_CurrentState;
	}

#ifdef WLD_PLATFORM_WINDOWS
	bool WindowsIme::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, ImeState& outState)
	{
		(void)wParam;
		switch (msg)
		{
		case WM_IME_STARTCOMPOSITION:
			s_CurrentState.Active = true;
			s_CurrentState.CompositionString.clear();
			outState = s_CurrentState;
			return true;

		case WM_IME_COMPOSITION:
		{
			HIMC himc = ImmGetContext(hwnd);
			if (himc)
			{
				if (lParam & GCS_COMPSTR)
				{
					LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
					if (bytes > 0)
					{
						s_CurrentState.CompositionString.resize(bytes / sizeof(wchar_t));
						ImmGetCompositionStringW(himc, GCS_COMPSTR, s_CurrentState.CompositionString.data(), bytes);
					}
					else
					{
						s_CurrentState.CompositionString.clear();
					}
				}
				if (lParam & GCS_CURSORPOS)
				{
					s_CurrentState.CursorPosition = ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0);
				}
				ImmReleaseContext(hwnd, himc);
			}
			outState = s_CurrentState;
			return true;
		}

		case WM_IME_ENDCOMPOSITION:
			s_CurrentState.Active = false;
			s_CurrentState.CompositionString.clear();
			outState = s_CurrentState;
			return true;
		}
		return false;
	}
#endif
}
