#pragma once

#include "World/Core/Export.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace World::Gameplay
{
	// 原始输入设备与键码(键码沿用引擎 KeyCodes 的整数值,不做二次映射)。
	enum class InputDevice : uint8_t
	{
		Key = 0,
		Mouse,
		Gamepad,
	};

	struct InputBinding
	{
		InputDevice Device = InputDevice::Key;
		int Code = 0;
	};

	struct InputAction
	{
		std::string Name;
		std::vector<InputBinding> Bindings;
	};

	// 轴 = 正/负两个动作 + 手柄轴(可选,如 "LX"/"LY")+ 死区。
	// 键盘与手柄都能驱动同一个轴,玩法代码只读 Axis("Move")。
	struct InputAxis
	{
		std::string Name;
		std::string PositiveAction;
		std::string NegativeAction;
		std::string GamepadAxis;      // 空 = 只用动作对
		float DeadZone = 0.15f;
	};

	// Input.weinput 资产(YAML):动作/轴/绑定。与 project.we.yaml 同级放置,Mod 可覆盖。
	class WLD_API InputMap
	{
	public:
		static bool Load(const std::filesystem::path& path, InputMap* out, std::string* error = nullptr);
		static bool Save(const std::filesystem::path& path, const InputMap& map, std::string* error = nullptr);

		const std::vector<InputAction>& Actions() const { return m_Actions; }
		std::vector<InputAction>& Actions() { return m_Actions; }
		const std::vector<InputAxis>& Axes() const { return m_Axes; }
		std::vector<InputAxis>& Axes() { return m_Axes; }

		const InputAction* FindAction(const std::string& name) const;
		const InputAxis* FindAxis(const std::string& name) const;

	private:
		std::vector<InputAction> m_Actions;
		std::vector<InputAxis> m_Axes;
	};

	// 输入服务(P2a W7):宿主每帧把原始设备状态喂进来,玩法/脚本只问动作与轴。
	// 多玩家:按键状态按"玩家槽位 + 设备 + 键码"记录,同一份映射可服务多个玩家。
	class WLD_API InputService
	{
	public:
		void SetMap(InputMap map) { m_Map = std::move(map); }
		const InputMap& GetMap() const { return m_Map; }

		void SetPlayerCount(uint32_t count);
		uint32_t GetPlayerCount() const { return static_cast<uint32_t>(m_Players.size()); }

		// 宿主每帧喂原始状态(未喂过的键视为抬起)。
		void SetKeyState(uint32_t player, InputDevice device, int code, bool down);
		void SetGamepadAxis(uint32_t player, const std::string& axis, float value);
		// 帧末调用:把"当前按下"滚成"上一帧按下",供 Pressed/Released 边沿判定。
		void EndFrame();
		void Clear();

		bool ActionDown(const std::string& action, uint32_t player = 0) const;
		bool ActionPressed(const std::string& action, uint32_t player = 0) const;
		bool ActionReleased(const std::string& action, uint32_t player = 0) const;
		float Axis(const std::string& axis, uint32_t player = 0) const;

		// 逐帧统计(可观测)。
		uint64_t GetEndFrameCount() const { return m_EndFrameCount; }

	private:
		struct PlayerState
		{
			std::unordered_map<uint64_t, bool> Down;
			std::unordered_map<uint64_t, bool> Previous;
			std::unordered_map<std::string, float> GamepadAxes;
		};

		static uint64_t MakeKey(InputDevice device, int code);
		bool BindingDown(const PlayerState& player, const InputAction& action) const;
		bool BindingPreviousDown(const PlayerState& player, const InputAction& action) const;
		const PlayerState* GetPlayer(uint32_t player) const;

		InputMap m_Map;
		std::vector<PlayerState> m_Players;
		uint64_t m_EndFrameCount = 0;
	};
}
