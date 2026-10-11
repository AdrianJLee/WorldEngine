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

	namespace
	{
		HWND s_DisabledHwnd = nullptr;
		HIMC s_PreviousHimc = nullptr;
	}

	void WindowsIme::SetImeEnabled(HWND hwnd, bool enabled)
	{
		if (!hwnd)
			return;

		if (!enabled)
		{
			// 1. 取消正在进行的任何输入法组合
			HIMC current = ImmGetContext(hwnd);
			if (current)
			{
				ImmNotifyIME(current, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
				ImmSetOpenStatus(current, FALSE);
				ImmReleaseContext(hwnd, current);
			}

			// 2. 操作系统级彻底脱钩输入法上下文 (ImmAssociateContext nullptr: 系统视其为无输入法原生游戏窗口)
			HIMC oldHimc = ImmAssociateContext(hwnd, nullptr);
			if (oldHimc && !s_PreviousHimc)
			{
				s_PreviousHimc = oldHimc;
				s_DisabledHwnd = hwnd;
			}
		}
		else
		{
			// 恢复输入法关联
			if (s_DisabledHwnd == hwnd && s_PreviousHimc)
			{
				ImmAssociateContext(hwnd, s_PreviousHimc);
				s_PreviousHimc = nullptr;
				s_DisabledHwnd = nullptr;
			}
			else
			{
				HIMC himc = ImmGetContext(hwnd);
				if (himc)
				{
					ImmSetOpenStatus(himc, TRUE);
					ImmReleaseContext(hwnd, himc);
				}
			}
		}
	}
#endif
}
