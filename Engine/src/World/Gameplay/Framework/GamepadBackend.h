#pragma once

#include "World/Core/Export.h"
#include "World/Gameplay/Framework/InputTypes.h"

#include <cstdint>
#include <string>

namespace World::Gameplay
{
	struct GamepadState
	{
		bool Connected = false;
		uint16_t Buttons = 0;   // 16 个标准按钮位图
		float Axes[6] = { 0.0f }; // LX, LY, RX, RY, LT, RT
	};

	// 手柄硬件抽象后端 (XInput 优先 + GLFW Gamepad 降级 + Headless Mock 支持)
	class WLD_API GamepadBackend
	{
	public:
		static GamepadBackend& Get();

		void Init();
		void Shutdown();
		bool Poll(uint32_t slot, GamepadState& outState);

		// 震动控制 (slot: 0..3, left/right motor: [0, 1])
		bool SetVibration(uint32_t slot, float leftMotor, float rightMotor);

		// Headless 与单元测试模拟控制
		void SetMockConnected(uint32_t slot, bool connected);
		void SetMockState(uint32_t slot, uint16_t buttons, const float axes[6]);
		void ClearMock();
		bool IsMockActive() const { return m_MockActive; }
		void GetLastVibration(uint32_t slot, float* outLeft, float* outRight) const;

	private:
		GamepadBackend();
		~GamepadBackend();
		GamepadBackend(const GamepadBackend&) = delete;
		GamepadBackend& operator=(const GamepadBackend&) = delete;

		void LoadXInput();

		bool m_Initialized = false;
		bool m_MockActive = false;
		GamepadState m_MockStates[4];
		float m_LastVibrationLeft[4] = { 0.0f };
		float m_LastVibrationRight[4] = { 0.0f };

		void* m_XInputModule = nullptr;
		void* m_XInputGetState = nullptr;
		void* m_XInputSetState = nullptr;
	};
}
