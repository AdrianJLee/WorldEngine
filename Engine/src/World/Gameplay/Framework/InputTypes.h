#pragma once

#include "World/Core/Export.h"
#include "World/Core/StringPool.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace World::Gameplay
{
	// 原始输入设备分类
	enum class InputDevice : uint8_t
	{
		Key = 0,
		Mouse,
		Gamepad,
		Touch
	};

	struct DeviceId
	{
		InputDevice Device = InputDevice::Key;
		uint8_t Index = 0; // 0..3 为手柄或触控槽位

		constexpr bool operator==(const DeviceId& other) const
		{
			return Device == other.Device && Index == other.Index;
		}
		constexpr bool operator!=(const DeviceId& other) const
		{
			return !(*this == other);
		}
	};

	// 动作值类型 (Button / 1D / 2D / 3D)
	enum class InputActionValueType : uint8_t
	{
		Button = 0, // 布尔动作 (跳跃, 开火)
		Axis1D,     // 浮点标量 (油门, 刹车)
		Axis2D,     // 二维向量 (移动 WASD, 视角摇杆)
		Axis3D      // 三维向量 (空间 6DOF)
	};

	// 动作触发生命周期
	enum class TriggerPhase : uint8_t
	{
		None = 0,
		Started,
		Ongoing,
		Triggered,
		Completed,
		Canceled
	};

	// 动作运行时状态 (POD, 零堆内存)
	struct ActionState
	{
		glm::vec3 Value{ 0.0f };
		float HeldTime = 0.0f;
		TriggerPhase Phase = TriggerPhase::None;
		bool Triggered = false;
		bool Down = false;
		bool Pressed = false;
		bool Released = false;
	};

	// 原始按键绑定
	struct InputBinding
	{
		InputDevice Device = InputDevice::Key;
		int Code = 0;
		uint8_t DeviceIndex = 0; // 手柄/设备槽位 (0..3)
	};

	// 动作定义 (包含驻留 NameId)
	struct InputAction
	{
		NameId Id;
		std::string Name;
		InputActionValueType ValueType = InputActionValueType::Button;
		std::vector<InputBinding> Bindings;
	};

	// 轴定义 (包含动作对或物理手柄轴)
	struct InputAxis
	{
		NameId Id;
		std::string Name;
		NameId PositiveActionId;
		NameId NegativeActionId;
		std::string PositiveAction;
		std::string NegativeAction;
		std::string GamepadAxis; // 如 "LX", "LY", "RX", "RY", "LT", "RT"
		float DeadZone = 0.15f;
	};

	// 定长、零堆、紧凑的原始物理设备状态 (双缓冲)
	struct RawInputState
	{
		// 键盘 512 键位图 (64 * 8)
		uint64_t Keys[8] = { 0 };

		// 鼠标 16 按钮位图
		uint16_t MouseButtons = 0;

		// 4 个手柄槽位: 16 按钮位图 + 6 模拟轴
		uint16_t GamepadButtons[4] = { 0 };
		float GamepadAxes[4][6] = { {0.0f} };
		bool GamepadConnected[4] = { false };

		// 鼠标指针与滚轮
		glm::vec2 MousePosition{ 0.0f };
		glm::vec2 MouseDelta{ 0.0f };
		glm::vec2 ScrollDelta{ 0.0f };

		// 键盘操作
		void SetKey(int code, bool down)
		{
			if (code >= 0 && code < 512)
			{
				const std::size_t bucket = static_cast<std::size_t>(code) / 64;
				const uint64_t mask = 1ULL << (code % 64);
				if (down)
					Keys[bucket] |= mask;
				else
					Keys[bucket] &= ~mask;
			}
		}

		bool IsKey(int code) const
		{
			if (code >= 0 && code < 512)
			{
				const std::size_t bucket = static_cast<std::size_t>(code) / 64;
				const uint64_t mask = 1ULL << (code % 64);
				return (Keys[bucket] & mask) != 0;
			}
			return false;
		}

		// 鼠标按键操作
		void SetMouseButton(int button, bool down)
		{
			if (button >= 0 && button < 16)
			{
				const uint16_t mask = static_cast<uint16_t>(1U << button);
				if (down)
					MouseButtons |= mask;
				else
					MouseButtons &= static_cast<uint16_t>(~mask);
			}
		}

		bool IsMouseButton(int button) const
		{
			if (button >= 0 && button < 16)
			{
				const uint16_t mask = static_cast<uint16_t>(1U << button);
				return (MouseButtons & mask) != 0;
			}
			return false;
		}

		// 手柄按键与轴操作
		void SetGamepadButton(uint8_t slot, int button, bool down)
		{
			if (slot < 4 && button >= 0 && button < 16)
			{
				const uint16_t mask = static_cast<uint16_t>(1U << button);
				if (down)
					GamepadButtons[slot] |= mask;
				else
					GamepadButtons[slot] &= static_cast<uint16_t>(~mask);
			}
		}

		bool IsGamepadButton(uint8_t slot, int button) const
		{
			if (slot < 4 && button >= 0 && button < 16)
			{
				const uint16_t mask = static_cast<uint16_t>(1U << button);
				return (GamepadButtons[slot] & mask) != 0;
			}
			return false;
		}

		void SetGamepadAxis(uint8_t slot, int axis, float value)
		{
			if (slot < 4 && axis >= 0 && axis < 6)
				GamepadAxes[slot][axis] = value;
		}

		float GetGamepadAxis(uint8_t slot, int axis) const
		{
			if (slot < 4 && axis >= 0 && axis < 6)
				return GamepadAxes[slot][axis];
			return 0.0f;
		}

		void Clear()
		{
			for (auto& k : Keys) k = 0;
			MouseButtons = 0;
			for (int i = 0; i < 4; ++i)
			{
				GamepadButtons[i] = 0;
				for (int a = 0; a < 6; ++a) GamepadAxes[i][a] = 0.0f;
				GamepadConnected[i] = false;
			}
			MousePosition = glm::vec2(0.0f);
			MouseDelta = glm::vec2(0.0f);
			ScrollDelta = glm::vec2(0.0f);
		}
	};
}
