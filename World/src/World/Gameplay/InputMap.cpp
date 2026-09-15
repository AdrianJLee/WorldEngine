#include "wldpch.h"
#include "World/Gameplay/InputMap.h"

#include "World/Core/Log.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>

namespace World::Gameplay
{
	namespace
	{
		const char* DeviceName(InputDevice device)
		{
			switch (device)
			{
			case InputDevice::Mouse:   return "mouse";
			case InputDevice::Gamepad: return "gamepad";
			default:                   return "key";
			}
		}

		InputDevice ParseDevice(const std::string& name)
		{
			if (name == "mouse") return InputDevice::Mouse;
			if (name == "gamepad") return InputDevice::Gamepad;
			return InputDevice::Key;
		}
	}

	bool InputMap::Load(const std::filesystem::path& path, InputMap* out, std::string* error)
	{
		if (!out)
			return false;
		if (!std::filesystem::exists(path))
		{
			if (error) *error = "input map not found: " + path.string();
			return false;
		}
		try
		{
			const YAML::Node root = YAML::LoadFile(path.string());
			InputMap parsed;
			for (const YAML::Node& entry : root["actions"])
			{
				InputAction action;
				if (entry["name"]) action.Name = entry["name"].as<std::string>();
				if (action.Name.empty())
				{
					if (error) *error = "input action requires a name: " + path.string();
					return false;
				}
				for (const YAML::Node& binding : entry["bindings"])
				{
					InputBinding parsedBinding;
					if (binding["device"]) parsedBinding.Device = ParseDevice(binding["device"].as<std::string>());
					if (binding["code"]) parsedBinding.Code = binding["code"].as<int>();
					action.Bindings.push_back(parsedBinding);
				}
				parsed.m_Actions.push_back(std::move(action));
			}
			for (const YAML::Node& entry : root["axes"])
			{
				InputAxis axis;
				if (entry["name"]) axis.Name = entry["name"].as<std::string>();
				if (entry["positive"]) axis.PositiveAction = entry["positive"].as<std::string>();
				if (entry["negative"]) axis.NegativeAction = entry["negative"].as<std::string>();
				if (entry["gamepad"]) axis.GamepadAxis = entry["gamepad"].as<std::string>();
				if (entry["deadzone"]) axis.DeadZone = entry["deadzone"].as<float>();
				if (axis.Name.empty())
				{
					if (error) *error = "input axis requires a name: " + path.string();
					return false;
				}
				parsed.m_Axes.push_back(std::move(axis));
			}
			*out = std::move(parsed);
			if (error) error->clear();
			return true;
		}
		catch (const std::exception& exception)
		{
			if (error) *error = std::string("input map parse failed: ") + exception.what();
			return false;
		}
	}

	bool InputMap::Save(const std::filesystem::path& path, const InputMap& map, std::string* error)
	{
		try
		{
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "actions" << YAML::Value << YAML::BeginSeq;
			for (const InputAction& action : map.m_Actions)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "name" << YAML::Value << action.Name;
				out << YAML::Key << "bindings" << YAML::Value << YAML::BeginSeq;
				for (const InputBinding& binding : action.Bindings)
				{
					out << YAML::BeginMap;
					out << YAML::Key << "device" << YAML::Value << DeviceName(binding.Device);
					out << YAML::Key << "code" << YAML::Value << binding.Code;
					out << YAML::EndMap;
				}
				out << YAML::EndSeq << YAML::EndMap;
			}
			out << YAML::EndSeq;
			out << YAML::Key << "axes" << YAML::Value << YAML::BeginSeq;
			for (const InputAxis& axis : map.m_Axes)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "name" << YAML::Value << axis.Name;
				out << YAML::Key << "positive" << YAML::Value << axis.PositiveAction;
				out << YAML::Key << "negative" << YAML::Value << axis.NegativeAction;
				out << YAML::Key << "gamepad" << YAML::Value << axis.GamepadAxis;
				out << YAML::Key << "deadzone" << YAML::Value << axis.DeadZone;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
			out << YAML::EndMap;

			std::filesystem::create_directories(path.parent_path());
			std::ofstream file(path);
			if (!file)
			{
				if (error) *error = "cannot write input map: " + path.string();
				return false;
			}
			file << out.c_str();
			if (error) error->clear();
			return true;
		}
		catch (const std::exception& exception)
		{
			if (error) *error = std::string("input map write failed: ") + exception.what();
			return false;
		}
	}

	const InputAction* InputMap::FindAction(const std::string& name) const
	{
		for (const InputAction& action : m_Actions)
			if (action.Name == name)
				return &action;
		return nullptr;
	}

	const InputAxis* InputMap::FindAxis(const std::string& name) const
	{
		for (const InputAxis& axis : m_Axes)
			if (axis.Name == name)
				return &axis;
		return nullptr;
	}

	// ---- InputService ----

	uint64_t InputService::MakeKey(InputDevice device, int code)
	{
		return (static_cast<uint64_t>(device) << 32) | static_cast<uint32_t>(code);
	}

	void InputService::SetPlayerCount(uint32_t count)
	{
		m_Players.resize(count);
	}

	void InputService::Clear()
	{
		m_Players.clear();
		m_EndFrameCount = 0;
	}

	void InputService::SetKeyState(uint32_t player, InputDevice device, int code, bool down)
	{
		if (player >= m_Players.size())
			m_Players.resize(player + 1);
		m_Players[player].Down[MakeKey(device, code)] = down;
	}

	void InputService::SetGamepadAxis(uint32_t player, const std::string& axis, float value)
	{
		if (player >= m_Players.size())
			m_Players.resize(player + 1);
		m_Players[player].GamepadAxes[axis] = value;
	}

	void InputService::EndFrame()
	{
		for (PlayerState& player : m_Players)
			player.Previous = player.Down;
		m_EndFrameCount++;
	}

	const InputService::PlayerState* InputService::GetPlayer(uint32_t player) const
	{
		return player < m_Players.size() ? &m_Players[player] : nullptr;
	}

	bool InputService::BindingDown(const PlayerState& player, const InputAction& action) const
	{
		for (const InputBinding& binding : action.Bindings)
		{
			const auto it = player.Down.find(MakeKey(binding.Device, binding.Code));
			if (it != player.Down.end() && it->second)
				return true;
		}
		return false;
	}

	bool InputService::BindingPreviousDown(const PlayerState& player, const InputAction& action) const
	{
		for (const InputBinding& binding : action.Bindings)
		{
			const auto it = player.Previous.find(MakeKey(binding.Device, binding.Code));
			if (it != player.Previous.end() && it->second)
				return true;
		}
		return false;
	}

	bool InputService::ActionDown(const std::string& action, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAction* definition = m_Map.FindAction(action);
		if (!state || !definition)
			return false;
		return BindingDown(*state, *definition);
	}

	bool InputService::ActionPressed(const std::string& action, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAction* definition = m_Map.FindAction(action);
		if (!state || !definition)
			return false;
		return BindingDown(*state, *definition) && !BindingPreviousDown(*state, *definition);
	}

	bool InputService::ActionReleased(const std::string& action, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAction* definition = m_Map.FindAction(action);
		if (!state || !definition)
			return false;
		return !BindingDown(*state, *definition) && BindingPreviousDown(*state, *definition);
	}

	float InputService::Axis(const std::string& axis, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAxis* definition = m_Map.FindAxis(axis);
		if (!state || !definition)
			return 0.0f;

		// 手柄轴优先(模拟量),否则用正/负动作对(数字量)。
		if (!definition->GamepadAxis.empty())
		{
			const auto it = state->GamepadAxes.find(definition->GamepadAxis);
			if (it != state->GamepadAxes.end())
			{
				const float value = std::fabs(it->second) < definition->DeadZone ? 0.0f : it->second;
				if (value != 0.0f)
					return std::clamp(value, -1.0f, 1.0f);
			}
		}

		float value = 0.0f;
		if (!definition->PositiveAction.empty())
			if (const InputAction* positive = m_Map.FindAction(definition->PositiveAction))
				if (BindingDown(*state, *positive))
					value += 1.0f;
		if (!definition->NegativeAction.empty())
			if (const InputAction* negative = m_Map.FindAction(definition->NegativeAction))
				if (BindingDown(*state, *negative))
					value -= 1.0f;
		return std::clamp(value, -1.0f, 1.0f);
	}
}
