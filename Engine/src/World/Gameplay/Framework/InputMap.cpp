#include "World/Gameplay/Framework/GamepadBackend.h"
#include "World/Events/DeviceEvent.h"
#include "wldpch.h"
#include "World/Gameplay/Framework/InputMap.h"

#include "World/Core/Log.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <fstream>

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
			case InputDevice::Touch:   return "touch";
			default:                   return "key";
			}
		}

		InputDevice ParseDevice(const std::string& name)
		{
			if (name == "mouse") return InputDevice::Mouse;
			if (name == "gamepad") return InputDevice::Gamepad;
			if (name == "touch") return InputDevice::Touch;
			return InputDevice::Key;
		}

		int ParseGamepadAxisIndex(const std::string& axis)
		{
			if (axis == "LX") return 0;
			if (axis == "LY") return 1;
			if (axis == "RX") return 2;
			if (axis == "RY") return 3;
			if (axis == "LT") return 4;
			if (axis == "RT") return 5;
			return -1;
		}
	}

	
	namespace
	{
		InputModifier ParseModifier(const YAML::Node& node)
		{
			InputModifier m;
			std::string type = node["type"] ? node["type"].as<std::string>() : "deadzone";
			if (type == "deadzone")
			{
				m.Type = ModifierType::DeadZone;
				if (node["lower"]) m.LowerThreshold = node["lower"].as<float>();
				if (node["upper"]) m.UpperThreshold = node["upper"].as<float>();
				if (node["radial"]) m.Radial = node["radial"].as<bool>();
			}
			else if (type == "invert")
			{
				m.Type = ModifierType::Invert;
				if (node["x"]) m.InvertX = node["x"].as<bool>();
				if (node["y"]) m.InvertY = node["y"].as<bool>();
				if (node["z"]) m.InvertZ = node["z"].as<bool>();
			}
			else if (type == "scale")
			{
				m.Type = ModifierType::Scale;
				if (node["factor"]) m.Scalar = glm::vec3(node["factor"].as<float>());
			}
			else if (type == "response_curve")
			{
				m.Type = ModifierType::ResponseCurve;
				if (node["exponent"]) m.Exponent = node["exponent"].as<float>();
			}
			else if (type == "swizzle")
			{
				m.Type = ModifierType::Swizzle;
				if (node["order"] && node["order"].IsSequence() && node["order"].size() >= 2)
				{
					m.SwizzleOrder[0] = node["order"][0].as<int>();
					m.SwizzleOrder[1] = node["order"][1].as<int>();
					if (node["order"].size() >= 3) m.SwizzleOrder[2] = node["order"][2].as<int>();
				}
			}
			else if (type == "normalize")
			{
				m.Type = ModifierType::Normalize;
				if (node["max"]) m.MaxLength = node["max"].as<float>();
			}
			return m;
		}

		InputTrigger ParseTrigger(const YAML::Node& node)
		{
			InputTrigger t;
			std::string type = node["type"] ? node["type"].as<std::string>() : "pressed";
			if (type == "pressed") t.Type = TriggerType::Pressed;
			else if (type == "released") t.Type = TriggerType::Released;
			else if (type == "hold")
			{
				t.Type = TriggerType::Hold;
				if (node["duration"]) t.Duration = node["duration"].as<float>();
			}
			else if (type == "tap")
			{
				t.Type = TriggerType::Tap;
				if (node["duration"]) t.Duration = node["duration"].as<float>();
			}
			else if (type == "double_tap")
			{
				t.Type = TriggerType::DoubleTap;
				if (node["interval"]) t.Interval = node["interval"].as<float>();
				if (node["duration"]) t.Duration = node["duration"].as<float>();
			}
			else if (type == "pulse")
			{
				t.Type = TriggerType::Pulse;
				if (node["interval"]) t.Interval = node["interval"].as<float>();
			}
			else if (type == "chord")
			{
				t.Type = TriggerType::Chord;
				if (node["chord_action"]) t.ChordAction = StringPool::Get().InternName(node["chord_action"].as<std::string>());
			}
			if (node["threshold"]) t.ActuationThreshold = node["threshold"].as<float>();
			return t;
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

			// 1. 读取基础 actions
			if (root["actions"])
			{
				for (const YAML::Node& entry : root["actions"])
				{
					InputAction action;
					if (entry["name"]) action.Name = entry["name"].as<std::string>();
					if (action.Name.empty())
					{
						if (error) *error = "input action requires a name: " + path.string();
						return false;
					}
					action.Id = StringPool::Get().InternName(action.Name);
					if (entry["type"])
					{
						std::string typeStr = entry["type"].as<std::string>();
						if (typeStr == "axis1d") action.ValueType = InputActionValueType::Axis1D;
						else if (typeStr == "axis2d") action.ValueType = InputActionValueType::Axis2D;
						else if (typeStr == "axis3d") action.ValueType = InputActionValueType::Axis3D;
						else action.ValueType = InputActionValueType::Button;
					}
					for (const YAML::Node& binding : entry["bindings"])
					{
						InputBinding parsedBinding;
						if (binding["device"]) parsedBinding.Device = ParseDevice(binding["device"].as<std::string>());
						if (binding["code"]) parsedBinding.Code = binding["code"].as<int>();
						if (binding["slot"]) parsedBinding.DeviceIndex = static_cast<uint8_t>(binding["slot"].as<int>());
						action.Bindings.push_back(parsedBinding);
					}
					parsed.m_Actions.push_back(std::move(action));
				}
			}

			// 2. 读取基础 axes
			if (root["axes"])
			{
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
					axis.Id = StringPool::Get().InternName(axis.Name);
					if (!axis.PositiveAction.empty())
						axis.PositiveActionId = StringPool::Get().InternName(axis.PositiveAction);
					if (!axis.NegativeAction.empty())
						axis.NegativeActionId = StringPool::Get().InternName(axis.NegativeAction);

					parsed.m_Axes.push_back(std::move(axis));
				}
			}

			// 3. 读取 v2 contexts (Mapping Contexts + Modifiers + Triggers)
			if (root["contexts"])
			{
				for (const YAML::Node& ctxNode : root["contexts"])
				{
					std::string ctxName = ctxNode["name"] ? ctxNode["name"].as<std::string>() : "Default";
					int32_t priority = ctxNode["priority"] ? ctxNode["priority"].as<int32_t>() : 0;
					bool consume = ctxNode["consume"] ? ctxNode["consume"].as<bool>() : true;

					InputMappingContext ctx(ctxName, priority, consume);
					if (ctxNode["mappings"])
					{
						for (const YAML::Node& mNode : ctxNode["mappings"])
						{
							ActionBindingConfig config;
							if (mNode["action"]) config.ActionName = mNode["action"].as<std::string>();
							config.Action = StringPool::Get().InternName(config.ActionName);

							if (mNode["binding"])
							{
								const auto& bNode = mNode["binding"];
								if (bNode["device"]) config.Binding.Device = ParseDevice(bNode["device"].as<std::string>());
								if (bNode["code"]) config.Binding.Code = bNode["code"].as<int>();
								if (bNode["slot"]) config.Binding.DeviceIndex = static_cast<uint8_t>(bNode["slot"].as<int>());
							}

							if (mNode["modifiers"])
							{
								for (const YAML::Node& modNode : mNode["modifiers"])
									config.Modifiers.push_back(ParseModifier(modNode));
							}

							if (mNode["triggers"])
							{
								for (const YAML::Node& trigNode : mNode["triggers"])
									config.Triggers.push_back(ParseTrigger(trigNode));
							}

							ctx.AddMapping(std::move(config));
						}
					}
					parsed.m_Contexts.push_back(std::move(ctx));
				}
			}

			parsed.RebuildIndices();
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
			out << YAML::Key << "version" << YAML::Value << 2;

			out << YAML::Key << "actions" << YAML::Value << YAML::BeginSeq;
			for (const InputAction& action : map.Actions())
			{
				out << YAML::BeginMap;
				out << YAML::Key << "name" << YAML::Value << action.Name;
				out << YAML::Key << "type" << YAML::Value << static_cast<int>(action.ValueType);
				out << YAML::Key << "bindings" << YAML::Value << YAML::BeginSeq;
				for (const InputBinding& binding : action.Bindings)
				{
					out << YAML::BeginMap;
					out << YAML::Key << "device" << YAML::Value << DeviceName(binding.Device);
					out << YAML::Key << "code" << YAML::Value << binding.Code;
					if (binding.DeviceIndex > 0)
						out << YAML::Key << "slot" << YAML::Value << static_cast<int>(binding.DeviceIndex);
					out << YAML::EndMap;
				}
				out << YAML::EndSeq << YAML::EndMap;
			}
			out << YAML::EndSeq;

			out << YAML::Key << "axes" << YAML::Value << YAML::BeginSeq;
			for (const InputAxis& axis : map.Axes())
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

			if (!map.Contexts().empty())
			{
				out << YAML::Key << "contexts" << YAML::Value << YAML::BeginSeq;
				for (const auto& ctx : map.Contexts())
				{
					out << YAML::BeginMap;
					out << YAML::Key << "name" << YAML::Value << ctx.GetName();
					out << YAML::Key << "priority" << YAML::Value << ctx.GetPriority();
					out << YAML::Key << "consume" << YAML::Value << ctx.ConsumesInput();

					out << YAML::Key << "mappings" << YAML::Value << YAML::BeginSeq;
					for (const auto& m : ctx.GetMappings())
					{
						out << YAML::BeginMap;
						out << YAML::Key << "action" << YAML::Value << m.ActionName;
						out << YAML::Key << "binding" << YAML::Value << YAML::BeginMap;
						out << YAML::Key << "device" << YAML::Value << DeviceName(m.Binding.Device);
						out << YAML::Key << "code" << YAML::Value << m.Binding.Code;
						if (m.Binding.DeviceIndex > 0)
							out << YAML::Key << "slot" << YAML::Value << static_cast<int>(m.Binding.DeviceIndex);
						out << YAML::EndMap;
						out << YAML::EndMap;
					}
					out << YAML::EndSeq;
					out << YAML::EndMap;
				}
				out << YAML::EndSeq;
			}

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

	void InputMap::RebuildIndices() const
	{
		m_ActionIdToIndex.clear();
		m_ActionNameToIndex.clear();
		for (std::size_t i = 0; i < m_Actions.size(); ++i)
		{
			InputAction& action = m_Actions[i];
			if (!action.Id.IsValid() && !action.Name.empty())
				action.Id = StringPool::Get().InternName(action.Name);
			if (action.Id.IsValid())
				m_ActionIdToIndex[action.Id.Value] = i;
			if (!action.Name.empty())
				m_ActionNameToIndex[action.Name] = i;
		}

		m_AxisIdToIndex.clear();
		m_AxisNameToIndex.clear();
		for (std::size_t i = 0; i < m_Axes.size(); ++i)
		{
			InputAxis& axis = m_Axes[i];
			if (!axis.Id.IsValid() && !axis.Name.empty())
				axis.Id = StringPool::Get().InternName(axis.Name);
			if (!axis.PositiveActionId.IsValid() && !axis.PositiveAction.empty())
				axis.PositiveActionId = StringPool::Get().InternName(axis.PositiveAction);
			if (!axis.NegativeActionId.IsValid() && !axis.NegativeAction.empty())
				axis.NegativeActionId = StringPool::Get().InternName(axis.NegativeAction);

			if (axis.Id.IsValid())
				m_AxisIdToIndex[axis.Id.Value] = i;
			if (!axis.Name.empty())
				m_AxisNameToIndex[axis.Name] = i;
		}

		m_IndicesDirty = false;
	}

	const InputAction* InputMap::FindAction(NameId id) const
	{
		EnsureIndices();
		const auto it = m_ActionIdToIndex.find(id.Value);
		return it != m_ActionIdToIndex.end() ? &m_Actions[it->second] : nullptr;
	}

	const InputAction* InputMap::FindAction(const std::string& name) const
	{
		EnsureIndices();
		const auto it = m_ActionNameToIndex.find(name);
		return it != m_ActionNameToIndex.end() ? &m_Actions[it->second] : nullptr;
	}

	const InputAxis* InputMap::FindAxis(NameId id) const
	{
		EnsureIndices();
		const auto it = m_AxisIdToIndex.find(id.Value);
		return it != m_AxisIdToIndex.end() ? &m_Axes[it->second] : nullptr;
	}

	const InputAxis* InputMap::FindAxis(const std::string& name) const
	{
		EnsureIndices();
		const auto it = m_AxisNameToIndex.find(name);
		return it != m_AxisNameToIndex.end() ? &m_Axes[it->second] : nullptr;
	}

	// ---- InputSnapshot ----

	bool InputSnapshot::IsDown(NameId action) const
	{
		const auto it = DownById.find(action.Value);
		return it != DownById.end() && it->second;
	}

	bool InputSnapshot::IsDown(const std::string& action) const
	{
		const auto it = Down.find(action);
		return it != Down.end() && it->second;
	}

	bool InputSnapshot::WasPressed(NameId action) const
	{
		const auto it = PressedById.find(action.Value);
		return it != PressedById.end() && it->second;
	}

	bool InputSnapshot::WasPressed(const std::string& action) const
	{
		const auto it = Pressed.find(action);
		return it != Pressed.end() && it->second;
	}

	bool InputSnapshot::WasReleased(NameId action) const
	{
		const auto it = ReleasedById.find(action.Value);
		return it != ReleasedById.end() && it->second;
	}

	bool InputSnapshot::WasReleased(const std::string& action) const
	{
		const auto it = Released.find(action);
		return it != Released.end() && it->second;
	}

	float InputSnapshot::GetAxis(NameId axis) const
	{
		const auto it = AxesById.find(axis.Value);
		return it != AxesById.end() ? it->second : 0.0f;
	}

	float InputSnapshot::GetAxis(const std::string& axis) const
	{
		const auto it = Axes.find(axis);
		return it != Axes.end() ? it->second : 0.0f;
	}

	// ---- InputService ----

	void InputService::SetMap(InputMap map)
	{
		m_Map = std::move(map);
		m_Map.RebuildIndices();
	}

	void InputService::SetPlayerCount(uint32_t count)
	{
		m_Players.resize(count);
	}

	void InputService::Clear()
	{
		m_Players.clear();
		m_Snapshot = {};
		m_EndFrameCount = 0;
	}

	InputService::PlayerState* InputService::GetOrCreatePlayer(uint32_t player)
	{
		if (player >= m_Players.size())
			m_Players.resize(player + 1);
		return &m_Players[player];
	}

	const InputService::PlayerState* InputService::GetPlayer(uint32_t player) const
	{
		return player < m_Players.size() ? &m_Players[player] : nullptr;
	}

	const RawInputState* InputService::GetRawState(uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		return state ? &state->Current : nullptr;
	}

	void InputService::SetKeyState(uint32_t player, InputDevice device, int code, bool down)
	{
		PlayerState* state = GetOrCreatePlayer(player);
		if (device == InputDevice::Key)
		{
			state->Current.SetKey(code, down);
		}
		else if (device == InputDevice::Mouse)
		{
			state->Current.SetMouseButton(code, down);
		}
		else if (device == InputDevice::Gamepad)
		{
			state->Current.SetGamepadButton(0, code, down);
		}
	}

	void InputService::SetGamepadAxis(uint32_t player, const std::string& axis, float value)
	{
		PlayerState* state = GetOrCreatePlayer(player);
		state->NamedGamepadAxes[axis] = value;
		const int index = ParseGamepadAxisIndex(axis);
		if (index >= 0)
			state->Current.SetGamepadAxis(0, index, value);
	}

	void InputService::SetGamepadAxis(uint32_t player, uint32_t axisIndex, float value)
	{
		PlayerState* state = GetOrCreatePlayer(player);
		if (axisIndex < 6)
			state->Current.SetGamepadAxis(0, static_cast<int>(axisIndex), value);
	}

	void InputService::EndFrame()
	{
		for (PlayerState& player : m_Players)
			player.Previous = player.Current;
		m_EndFrameCount++;
	}

	void InputService::BuildSnapshot(uint32_t player)
	{
		m_Snapshot = {};
		for (const InputAction& action : m_Map.Actions())
		{
			const bool down = ActionDown(action.Id, player);
			const bool pressed = ActionPressed(action.Id, player);
			const bool released = ActionReleased(action.Id, player);

			m_Snapshot.Down[action.Name] = down;
			m_Snapshot.Pressed[action.Name] = pressed;
			m_Snapshot.Released[action.Name] = released;

			m_Snapshot.DownById[action.Id.Value] = down;
			m_Snapshot.PressedById[action.Id.Value] = pressed;
			m_Snapshot.ReleasedById[action.Id.Value] = released;
		}
		for (const InputAxis& axis : m_Map.Axes())
		{
			const float value = Axis(axis.Id, player);
			m_Snapshot.Axes[axis.Name] = value;
			m_Snapshot.AxesById[axis.Id.Value] = value;
		}
	}

	bool InputService::BindingDown(const PlayerState& player, const InputAction& action) const
	{
		for (const InputBinding& binding : action.Bindings)
		{
			if (binding.Device == InputDevice::Key)
			{
				if (player.Current.IsKey(binding.Code))
					return true;
			}
			else if (binding.Device == InputDevice::Mouse)
			{
				if (player.Current.IsMouseButton(binding.Code))
					return true;
			}
			else if (binding.Device == InputDevice::Gamepad)
			{
				if (player.Current.IsGamepadButton(binding.DeviceIndex, binding.Code))
					return true;
			}
		}
		return false;
	}

	bool InputService::BindingPreviousDown(const PlayerState& player, const InputAction& action) const
	{
		for (const InputBinding& binding : action.Bindings)
		{
			if (binding.Device == InputDevice::Key)
			{
				if (player.Previous.IsKey(binding.Code))
					return true;
			}
			else if (binding.Device == InputDevice::Mouse)
			{
				if (player.Previous.IsMouseButton(binding.Code))
					return true;
			}
			else if (binding.Device == InputDevice::Gamepad)
			{
				if (player.Previous.IsGamepadButton(binding.DeviceIndex, binding.Code))
					return true;
			}
		}
		return false;
	}

	bool InputService::ActionDown(NameId action, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAction* definition = m_Map.FindAction(action);
		if (!state || !definition)
			return false;
		return BindingDown(*state, *definition);
	}

	bool InputService::ActionDown(const std::string& action, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAction* definition = m_Map.FindAction(action);
		if (!state || !definition)
			return false;
		return BindingDown(*state, *definition);
	}

	bool InputService::ActionPressed(NameId action, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAction* definition = m_Map.FindAction(action);
		if (!state || !definition)
			return false;
		return BindingDown(*state, *definition) && !BindingPreviousDown(*state, *definition);
	}

	bool InputService::ActionPressed(const std::string& action, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAction* definition = m_Map.FindAction(action);
		if (!state || !definition)
			return false;
		return BindingDown(*state, *definition) && !BindingPreviousDown(*state, *definition);
	}

	bool InputService::ActionReleased(NameId action, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAction* definition = m_Map.FindAction(action);
		if (!state || !definition)
			return false;
		return !BindingDown(*state, *definition) && BindingPreviousDown(*state, *definition);
	}

	bool InputService::ActionReleased(const std::string& action, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAction* definition = m_Map.FindAction(action);
		if (!state || !definition)
			return false;
		return !BindingDown(*state, *definition) && BindingPreviousDown(*state, *definition);
	}

	float InputService::Axis(NameId axis, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAxis* definition = m_Map.FindAxis(axis);
		if (!state || !definition)
			return 0.0f;

		// 手柄轴优先(模拟量),否则用正/负动作对(数字量)。
		if (!definition->GamepadAxis.empty())
		{
			const int axisIndex = ParseGamepadAxisIndex(definition->GamepadAxis);
			if (axisIndex >= 0)
			{
				const float val = state->Current.GetGamepadAxis(0, axisIndex);
				const float filtered = std::fabs(val) < definition->DeadZone ? 0.0f : val;
				if (filtered != 0.0f)
					return std::clamp(filtered, -1.0f, 1.0f);
			}

			const auto it = state->NamedGamepadAxes.find(definition->GamepadAxis);
			if (it != state->NamedGamepadAxes.end())
			{
				const float val = it->second;
				const float filtered = std::fabs(val) < definition->DeadZone ? 0.0f : val;
				if (filtered != 0.0f)
					return std::clamp(filtered, -1.0f, 1.0f);
			}
		}

		float value = 0.0f;
		if (definition->PositiveActionId.IsValid())
		{
			if (const InputAction* positive = m_Map.FindAction(definition->PositiveActionId))
				if (BindingDown(*state, *positive))
					value += 1.0f;
		}
		else if (!definition->PositiveAction.empty())
		{
			if (const InputAction* positive = m_Map.FindAction(definition->PositiveAction))
				if (BindingDown(*state, *positive))
					value += 1.0f;
		}

		if (definition->NegativeActionId.IsValid())
		{
			if (const InputAction* negative = m_Map.FindAction(definition->NegativeActionId))
				if (BindingDown(*state, *negative))
					value -= 1.0f;
		}
		else if (!definition->NegativeAction.empty())
		{
			if (const InputAction* negative = m_Map.FindAction(definition->NegativeAction))
				if (BindingDown(*state, *negative))
					value -= 1.0f;
		}

		return std::clamp(value, -1.0f, 1.0f);
	}

	float InputService::Axis(const std::string& axis, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		const InputAxis* definition = m_Map.FindAxis(axis);
		if (!state || !definition)
			return 0.0f;

		return Axis(definition->Id, player);
	}


	void InputService::BindPlayerDevice(uint32_t player, DeviceId device)
	{
		PlayerState* p = GetOrCreatePlayer(player);
		p->BoundDevice = device;
	}

	DeviceId InputService::GetPlayerDevice(uint32_t player) const
	{
		const PlayerState* p = GetPlayer(player);
		return p ? p->BoundDevice : DeviceId{ InputDevice::Key, 0 };
	}

	int32_t InputService::FindPlayerForDevice(DeviceId device) const
	{
		for (uint32_t i = 0; i < static_cast<uint32_t>(m_Players.size()); ++i)
		{
			if (m_Players[i].BoundDevice == device)
				return static_cast<int32_t>(i);
		}
		return -1;
	}

	void InputService::SetVibration(uint32_t player, float leftMotor, float rightMotor, float durationSec)
	{
		PlayerState* p = GetOrCreatePlayer(player);
		p->Vibration.LeftMotor = std::clamp(leftMotor, 0.0f, 1.0f);
		p->Vibration.RightMotor = std::clamp(rightMotor, 0.0f, 1.0f);
		p->Vibration.RemainingDuration = durationSec;

		// 默认将手柄槽位映射为 player 槽位 (0..3)
		if (player < 4)
			GamepadBackend::Get().SetVibration(player, p->Vibration.LeftMotor, p->Vibration.RightMotor);
	}

	void InputService::GetVibration(uint32_t player, float* outLeft, float* outRight) const
	{
		const PlayerState* p = GetPlayer(player);
		if (p)
		{
			if (outLeft) *outLeft = p->Vibration.LeftMotor;
			if (outRight) *outRight = p->Vibration.RightMotor;
		}
		else
		{
			if (outLeft) *outLeft = 0.0f;
			if (outRight) *outRight = 0.0f;
		}
	}

	void InputService::UpdateVibration(float dt)
	{
		for (uint32_t i = 0; i < static_cast<uint32_t>(m_Players.size()); ++i)
		{
			PlayerState& p = m_Players[i];
			if (p.Vibration.RemainingDuration > 0.0f)
			{
				p.Vibration.RemainingDuration -= dt;
				if (p.Vibration.RemainingDuration <= 0.0f)
				{
					p.Vibration.LeftMotor = 0.0f;
					p.Vibration.RightMotor = 0.0f;
					p.Vibration.RemainingDuration = 0.0f;
					if (i < 4)
						GamepadBackend::Get().SetVibration(i, 0.0f, 0.0f);
				}
			}
		}
	}

	void InputService::PollDevices()
	{
		if (m_Players.empty())
			SetPlayerCount(1);

		for (uint32_t slot = 0; slot < 4; ++slot)
		{
			GamepadState state;
			const bool connected = GamepadBackend::Get().Poll(slot, state);
			if (connected != m_PrevGamepadConnected[slot])
			{
				m_PrevGamepadConnected[slot] = connected;
				DeviceId devId{ InputDevice::Gamepad, static_cast<uint8_t>(slot) };
				if (connected)
				{
					DeviceConnectedEvent ev(devId);
					if (m_EventCallback) m_EventCallback(ev);
				}
				else
				{
					DeviceDisconnectedEvent ev(devId);
					if (m_EventCallback) m_EventCallback(ev);
				}
			}

			if (connected)
			{
				// 若开启自动认领且该手柄未配对, 当产生输入时认领至空闲玩家
				if (m_PairingMode == DevicePairingMode::AutoClaim && (state.Buttons != 0 || std::fabs(state.Axes[0]) > 0.2f))
				{
					DeviceId devId{ InputDevice::Gamepad, static_cast<uint8_t>(slot) };
					if (FindPlayerForDevice(devId) < 0)
					{
						// 绑定到对应槽位的玩家或第一个无绑定的玩家
						if (slot < m_Players.size())
							BindPlayerDevice(slot, devId);
					}
				}

				// 将物理手柄状态直接喂入对应玩家或全部包含该绑定的槽位
				int32_t playerIdx = FindPlayerForDevice(DeviceId{ InputDevice::Gamepad, static_cast<uint8_t>(slot) });
				uint32_t targetPlayer = playerIdx >= 0 ? static_cast<uint32_t>(playerIdx) : slot;
				if (targetPlayer < m_Players.size())
				{
					PlayerState& p = m_Players[targetPlayer];
					p.Current.GamepadButtons[slot] = state.Buttons;
					for (int a = 0; a < 6; ++a)
						p.Current.GamepadAxes[slot][a] = state.Axes[a];
					p.Current.GamepadConnected[slot] = true;
				}
			}
		}
	}


	void InputService::PushContext(InputMappingContext context, int32_t priority)
	{
		context.SetPriority(priority);
		// 按优先级从高到低插入
		auto it = std::upper_bound(m_ContextStack.begin(), m_ContextStack.end(), context,
			[](const InputMappingContext& a, const InputMappingContext& b) {
				return a.GetPriority() > b.GetPriority();
			});
		m_ContextStack.insert(it, std::move(context));
	}

	void InputService::PopContext(NameId contextId)
	{
		for (auto it = m_ContextStack.begin(); it != m_ContextStack.end(); ++it)
		{
			if (it->GetId() == contextId)
			{
				m_ContextStack.erase(it);
				break;
			}
		}
	}

	void InputService::PopContext(const std::string& contextName)
	{
		PopContext(StringPool::Get().InternName(contextName));
	}

	bool InputService::HasContext(NameId contextId) const
	{
		for (const auto& ctx : m_ContextStack)
		{
			if (ctx.GetId() == contextId)
				return true;
		}
		return false;
	}

	void InputService::ClearContexts()
	{
		m_ContextStack.clear();
	}

	ActionState InputService::EvaluateActionState(NameId action, uint32_t player, float dt) const
	{
		ActionState state;
		const PlayerState* pState = GetPlayer(player);
		if (!pState)
			return state;

		// 1. 如果上下文栈为空, 回退到基础映射求值
		if (m_ContextStack.empty())
		{
			state.Down = ActionDown(action, player);
			state.Pressed = ActionPressed(action, player);
			state.Released = ActionReleased(action, player);
			state.Triggered = state.Pressed;
			state.Phase = state.Triggered ? TriggerPhase::Triggered : (state.Down ? TriggerPhase::Ongoing : TriggerPhase::None);
			const float ax = Axis(action, player);
			state.Value = glm::vec3(ax, 0.0f, 0.0f);
			return state;
		}

		// 2. 按上下文栈优先级求值
		for (const auto& ctx : m_ContextStack)
		{
			const auto mappings = ctx.FindMappings(action);
			for (const ActionBindingConfig* config : mappings)
			{
				bool bindingDown = false;
				float rawVal = 0.0f;
				if (config->Binding.Device == InputDevice::Key)
				{
					bindingDown = pState->Current.IsKey(config->Binding.Code);
					rawVal = bindingDown ? 1.0f : 0.0f;
				}
				else if (config->Binding.Device == InputDevice::Mouse)
				{
					bindingDown = pState->Current.IsMouseButton(config->Binding.Code);
					rawVal = bindingDown ? 1.0f : 0.0f;
				}
				else if (config->Binding.Device == InputDevice::Gamepad)
				{
					bindingDown = pState->Current.IsGamepadButton(config->Binding.DeviceIndex, config->Binding.Code);
					rawVal = bindingDown ? 1.0f : 0.0f;
				}

				// 应用 Modifiers
				glm::vec3 val{ rawVal, 0.0f, 0.0f };
				for (const auto& mod : config->Modifiers)
					val = mod.Apply(val, dt);

				// 应用 Triggers
				TriggerState& trigState = pState->TriggerStates[action.Value];
				TriggerPhase phase = TriggerPhase::None;
				if (!config->Triggers.empty())
				{
					for (const auto& trig : config->Triggers)
					{
						phase = trig.Evaluate(bindingDown, dt, trigState);
						if (phase == TriggerPhase::Triggered)
							break;
					}
				}
				else
				{
					phase = bindingDown ? (trigState.WasActuated ? TriggerPhase::Ongoing : TriggerPhase::Triggered) : TriggerPhase::None;
					trigState.WasActuated = bindingDown;
				}

				if (phase == TriggerPhase::Triggered || phase == TriggerPhase::Ongoing)
				{
					state.Value = val;
					state.Phase = phase;
					state.Triggered = (phase == TriggerPhase::Triggered);
					state.Down = bindingDown;
					state.HeldTime = trigState.HeldTime;
					return state;
				}
			}

			// 如果上下文消费输入且已匹配，则拦截
			if (ctx.ConsumesInput() && !mappings.empty())
				break;
		}

		return state;
	}

}
