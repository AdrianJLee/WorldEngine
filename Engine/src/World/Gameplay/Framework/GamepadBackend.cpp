#include "wldpch.h"
#include "World/Gameplay/Framework/GamepadBackend.h"

#include <algorithm>
#include <cmath>

#ifdef WLD_PLATFORM_WINDOWS
#include <windows.h>
#include <xinput.h>
#endif

#include <GLFW/glfw3.h>

namespace World::Gameplay
{
#ifdef WLD_PLATFORM_WINDOWS
	typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, XINPUT_STATE*);
	typedef DWORD(WINAPI* PFN_XInputSetState)(DWORD, XINPUT_VIBRATION*);
#endif

	GamepadBackend& GamepadBackend::Get()
	{
		static GamepadBackend instance;
		return instance;
	}

	GamepadBackend::GamepadBackend()
	{
		Init();
	}

	GamepadBackend::~GamepadBackend()
	{
		Shutdown();
	}

	void GamepadBackend::Init()
	{
		if (m_Initialized)
			return;
		m_Initialized = true;
		ClearMock();
		LoadXInput();
	}

	void GamepadBackend::Shutdown()
	{
#ifdef WLD_PLATFORM_WINDOWS
		if (m_XInputModule)
		{
			FreeLibrary(static_cast<HMODULE>(m_XInputModule));
			m_XInputModule = nullptr;
			m_XInputGetState = nullptr;
			m_XInputSetState = nullptr;
		}
#endif
		m_Initialized = false;
	}

	void GamepadBackend::LoadXInput()
	{
#ifdef WLD_PLATFORM_WINDOWS
		if (m_XInputModule)
			return;

		HMODULE module = LoadLibraryW(L"xinput1_4.dll");
		if (!module)
			module = LoadLibraryW(L"xinput9_1_0.dll");
		if (!module)
			module = LoadLibraryW(L"xinput1_3.dll");

		if (module)
		{
			m_XInputModule = module;
			m_XInputGetState = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState"));
			m_XInputSetState = reinterpret_cast<void*>(GetProcAddress(module, "XInputSetState"));
		}
#endif
	}

	bool GamepadBackend::Poll(uint32_t slot, GamepadState& outState)
	{
		if (slot >= 4)
			return false;

		if (m_MockActive)
		{
			outState = m_MockStates[slot];
			return outState.Connected;
		}

#ifdef WLD_PLATFORM_WINDOWS
		if (m_XInputGetState)
		{
			auto pfnGetState = reinterpret_cast<PFN_XInputGetState>(m_XInputGetState);
			XINPUT_STATE xstate;
			ZeroMemory(&xstate, sizeof(XINPUT_STATE));
			if (pfnGetState(static_cast<DWORD>(slot), &xstate) == ERROR_SUCCESS)
			{
				outState.Connected = true;
				outState.Buttons = xstate.Gamepad.wButtons;

				// 左右摇杆归一化 [-1, 1]
				auto normStick = [](SHORT val) -> float {
					if (val < 0)
						return static_cast<float>(val) / 32768.0f;
					return static_cast<float>(val) / 32767.0f;
				};

				outState.Axes[0] = normStick(xstate.Gamepad.sThumbLX);
				outState.Axes[1] = normStick(xstate.Gamepad.sThumbLY);
				outState.Axes[2] = normStick(xstate.Gamepad.sThumbRX);
				outState.Axes[3] = normStick(xstate.Gamepad.sThumbRY);

				// 扳机归一化 [0, 1]
				outState.Axes[4] = static_cast<float>(xstate.Gamepad.bLeftTrigger) / 255.0f;
				outState.Axes[5] = static_cast<float>(xstate.Gamepad.bRightTrigger) / 255.0f;
				return true;
			}
		}
#endif

		// GLFW Gamepad 回退
		const int jid = static_cast<int>(slot);
		if (glfwJoystickPresent(jid) && glfwJoystickIsGamepad(jid))
		{
			GLFWgamepadstate gstate;
			if (glfwGetGamepadState(jid, &gstate))
			{
				outState.Connected = true;
				outState.Buttons = 0;
				for (int b = 0; b < 15; ++b)
				{
					if (gstate.buttons[b] == GLFW_PRESS)
						outState.Buttons |= static_cast<uint16_t>(1U << b);
				}
				outState.Axes[0] = gstate.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
				outState.Axes[1] = -gstate.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
				outState.Axes[2] = gstate.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
				outState.Axes[3] = -gstate.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
				outState.Axes[4] = (gstate.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f;
				outState.Axes[5] = (gstate.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;
				return true;
			}
		}

		outState.Connected = false;
		outState.Buttons = 0;
		for (int a = 0; a < 6; ++a)
			outState.Axes[a] = 0.0f;
		return false;
	}

	bool GamepadBackend::SetVibration(uint32_t slot, float leftMotor, float rightMotor)
	{
		if (slot >= 4)
			return false;

		m_LastVibrationLeft[slot] = leftMotor;
		m_LastVibrationRight[slot] = rightMotor;

		if (m_MockActive)
			return true;

#ifdef WLD_PLATFORM_WINDOWS
		if (m_XInputSetState)
		{
			auto pfnSetState = reinterpret_cast<PFN_XInputSetState>(m_XInputSetState);
			XINPUT_VIBRATION vib;
			vib.wLeftMotorSpeed = static_cast<WORD>(std::clamp(leftMotor, 0.0f, 1.0f) * 65535.0f);
			vib.wRightMotorSpeed = static_cast<WORD>(std::clamp(rightMotor, 0.0f, 1.0f) * 65535.0f);
			return pfnSetState(static_cast<DWORD>(slot), &vib) == ERROR_SUCCESS;
		}
#endif
		return false;
	}

	void GamepadBackend::SetMockConnected(uint32_t slot, bool connected)
	{
		if (slot < 4)
		{
			m_MockActive = true;
			m_MockStates[slot].Connected = connected;
		}
	}

	void GamepadBackend::SetMockState(uint32_t slot, uint16_t buttons, const float axes[6])
	{
		if (slot < 4)
		{
			m_MockActive = true;
			m_MockStates[slot].Connected = true;
			m_MockStates[slot].Buttons = buttons;
			if (axes)
			{
				for (int i = 0; i < 6; ++i)
					m_MockStates[slot].Axes[i] = axes[i];
			}
		}
	}

	void GamepadBackend::ClearMock()
	{
		m_MockActive = false;
		for (int i = 0; i < 4; ++i)
		{
			m_MockStates[i].Connected = false;
			m_MockStates[i].Buttons = 0;
			for (int a = 0; a < 6; ++a)
				m_MockStates[i].Axes[a] = 0.0f;
			m_LastVibrationLeft[i] = 0.0f;
			m_LastVibrationRight[i] = 0.0f;
		}
	}

	void GamepadBackend::GetLastVibration(uint32_t slot, float* outLeft, float* outRight) const
	{
		if (slot < 4)
		{
			if (outLeft) *outLeft = m_LastVibrationLeft[slot];
			if (outRight) *outRight = m_LastVibrationRight[slot];
		}
	}
}
