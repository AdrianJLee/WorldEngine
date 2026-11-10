#include "wldpch.h"
#include "World/Platform/Windows/WindowsIme.h"

#ifdef WLD_PLATFORM_WINDOWS
#include <imm.h>
#include <unordered_map>
#endif

namespace World::Platform
{
	namespace
	{
		ImeState s_CurrentState;

#ifdef WLD_PLATFORM_WINDOWS
		struct WindowImeData
		{
			HIMC SavedHimc = nullptr;
			bool Enabled = true; // Windows OS 默认新建 HWND 为启用输入法
		};

		static std::unordered_map<HWND, WindowImeData> s_WindowStates;
#endif
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

	void WindowsIme::SetImeEnabled(HWND hwnd, bool enabled)
	{
		if (!hwnd)
			return;

		auto& data = s_WindowStates[hwnd];

		if (!enabled)
		{
			// 若当前已经记录为禁用，且系统未意外重新关联上下文，则直接返回（保持零开销）
			if (!data.Enabled)
			{
				HIMC current = ImmGetContext(hwnd);
				if (!current)
					return;
				ImmNotifyIME(current, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
				ImmSetOpenStatus(current, FALSE);
				ImmReleaseContext(hwnd, current);
			}
			else
			{
				// 1. 取消正在进行的任何输入法组合
				HIMC current = ImmGetContext(hwnd);
				if (current)
				{
					ImmNotifyIME(current, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
					ImmSetOpenStatus(current, FALSE);
					ImmReleaseContext(hwnd, current);
				}
			}

			// 2. 操作系统级彻底脱钩输入法上下文 (ImmAssociateContext nullptr: 系统视其为无输入法原生游戏窗口)
			HIMC oldHimc = ImmAssociateContext(hwnd, nullptr);
			if (oldHimc)
			{
				data.SavedHimc = oldHimc;
			}
			data.Enabled = false;
		}
		else
		{
			if (data.Enabled)
			{
				// 已处于启用状态，确保打开状态为 TRUE
				HIMC current = ImmGetContext(hwnd);
				if (current)
				{
					ImmSetOpenStatus(current, TRUE);
					ImmReleaseContext(hwnd, current);
				}
				return;
			}

			// 恢复输入法关联
			if (data.SavedHimc)
			{
				ImmAssociateContext(hwnd, data.SavedHimc);
			}
			else
			{
				ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
			}

			HIMC himc = ImmGetContext(hwnd);
			if (himc)
			{
				ImmSetOpenStatus(himc, TRUE);
				ImmReleaseContext(hwnd, himc);
			}
			data.Enabled = true;
		}
	}

	bool WindowsIme::IsImeEnabled(HWND hwnd)
	{
		if (!hwnd)
			return false;

		auto it = s_WindowStates.find(hwnd);
		if (it != s_WindowStates.end())
			return it->second.Enabled;

		HIMC current = ImmGetContext(hwnd);
		if (current)
		{
			ImmReleaseContext(hwnd, current);
			return true;
		}
		return false;
	}

	void WindowsIme::OnWindowDestroyed(HWND hwnd)
	{
		if (!hwnd)
			return;

		auto it = s_WindowStates.find(hwnd);
		if (it != s_WindowStates.end())
		{
			if (it->second.SavedHimc)
			{
				ImmAssociateContext(hwnd, it->second.SavedHimc);
			}
			s_WindowStates.erase(it);
		}
	}
#endif
}