#include "World/Gameplay/Framework/GamepadBackend.h"
#include "World/Events/DeviceEvent.h"
#include "wldpch.h"
#include "World/Gameplay/Framework/InputMap.h"

#include "World/Core/Log.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <tuple>

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
					// R1:`default: true` = 基础上下文,装载后自动压栈(见 InputService::SetMap)。
					// 没标这个位的上下文(载具/瞄准/菜单…)由游戏代码或脚本 PushContext/PopContext。
					const bool autoPush = ctxNode["default"] ? ctxNode["default"].as<bool>() : false;

					InputMappingContext ctx(ctxName, priority, consume);
					ctx.SetAutoPush(autoPush);
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

			// R1:装载期诊断。此前"文件写错了"与"文件读到了"在日志上完全一样,而查询侧对未知
			// 动作一律返回 false/0(该容错本身是对的)⇒ 组合出"永远读不到、永远不报错"的静默失效
			// (同一子系统上已连续踩到三次)。这里把可判定的错误在装载时一次说清。
			if (Log::GetCoreLogger())
			{
				if (parsed.m_Actions.empty())
					WLD_CORE_WARN("[input] '{0}' declares no action: every Input.* query returns false/0",
						path.string());
				for (const InputAction& action : parsed.m_Actions)
				{
					if (!action.Bindings.empty())
						continue;
					bool mappedSomewhere = false;
					for (const InputMappingContext& context : parsed.m_Contexts)
						if (!context.FindMappings(action.Id).empty())
						{
							mappedSomewhere = true;
							break;
						}
					if (!mappedSomewhere)
						WLD_CORE_WARN("[input] action '{0}' is declared but never bound "
							"(no bindings and no context mapping): it can never be triggered",
							action.Name);
				}
				for (const InputMappingContext& context : parsed.m_Contexts)
				{
					// (device, code, action) 三元组 —— **同一个键绑给不同动作是合法的**
					// (点按=跳 / 双击=翻滚 就是这么写的,两个动作各自独立解算),所以只报
					// "同一动作在同一上下文里被绑了两次"这种纯冗余数据。
					std::vector<std::tuple<int, int, uint32_t>> seen;
					for (const ActionBindingConfig& mapping : context.GetMappings())
					{
						if (mapping.ActionName.empty() || !parsed.FindAction(mapping.Action))
							WLD_CORE_WARN("[input] context '{0}' maps unknown action '{1}' "
								"(no such entry under actions:)", context.GetName(), mapping.ActionName);
						const std::tuple<int, int, uint32_t> key{ static_cast<int>(mapping.Binding.Device),
							mapping.Binding.Code, mapping.Action.Value };
						if (std::find(seen.begin(), seen.end(), key) != seen.end())
							WLD_CORE_WARN("[input] context '{0}' binds action '{1}' to ({2}, {3}) more than once "
								"(redundant mapping)", context.GetName(), mapping.ActionName,
								DeviceName(mapping.Binding.Device), mapping.Binding.Code);
						else
							seen.push_back(key);
					}
				}
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

	namespace
	{
		// M40:Save 侧的名字映射与字段发射,与上面的 ParseTrigger / ParseModifier 逐字段对称。
		// 此前 Save **完全不写** modifiers / triggers(整个函数里 "Trigger" 出现 0 次),
		// 于是编辑器 Input Map 面板保存一次就把它们连同注释一起抹掉 ——
		// 实测:模板工程只重绑一个键,5 个 trigger 块全部消失(2232 -> 1523 字节)。
		const char* TriggerTypeName(TriggerType type)
		{
			switch (type)
			{
			case TriggerType::Pressed: return "pressed";
			case TriggerType::Released: return "released";
			case TriggerType::Hold: return "hold";
			case TriggerType::Tap: return "tap";
			case TriggerType::DoubleTap: return "double_tap";
			case TriggerType::Pulse: return "pulse";
			case TriggerType::Chord: return "chord";
			}
			return "pressed";
		}

		const char* ModifierTypeName(ModifierType type)
		{
			switch (type)
			{
			case ModifierType::DeadZone: return "deadzone";
			case ModifierType::Invert: return "invert";
			case ModifierType::Scale: return "scale";
			case ModifierType::ResponseCurve: return "response_curve";
			case ModifierType::Swizzle: return "swizzle";
			case ModifierType::Normalize: return "normalize";
			}
			return "deadzone";
		}

		// 只写该类型真正消费的参数(与对应 Parse* 读的键一一对应);threshold 等于默认 0.5 时省略,
		// 于是"手写的原始文件"过一次保存后语义不变、噪声最小。
		void WriteTrigger(YAML::Emitter& out, const InputTrigger& trigger)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "type" << YAML::Value << TriggerTypeName(trigger.Type);
			switch (trigger.Type)
			{
			case TriggerType::Hold:
			case TriggerType::Tap:
				out << YAML::Key << "duration" << YAML::Value << trigger.Duration;
				break;
			case TriggerType::DoubleTap:
				out << YAML::Key << "interval" << YAML::Value << trigger.Interval;
				out << YAML::Key << "duration" << YAML::Value << trigger.Duration;
				break;
			case TriggerType::Pulse:
				out << YAML::Key << "interval" << YAML::Value << trigger.Interval;
				break;
			case TriggerType::Chord:
				if (trigger.ChordAction.IsValid())
					out << YAML::Key << "chord_action" << YAML::Value
						<< StringPool::Get().NameOf(trigger.ChordAction);
				break;
			default:
				break;
			}
			if (trigger.ActuationThreshold != 0.5f)
				out << YAML::Key << "threshold" << YAML::Value << trigger.ActuationThreshold;
			out << YAML::EndMap;
		}

		void WriteModifier(YAML::Emitter& out, const InputModifier& modifier)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "type" << YAML::Value << ModifierTypeName(modifier.Type);
			switch (modifier.Type)
			{
			case ModifierType::DeadZone:
				out << YAML::Key << "lower" << YAML::Value << modifier.LowerThreshold;
				out << YAML::Key << "upper" << YAML::Value << modifier.UpperThreshold;
				out << YAML::Key << "radial" << YAML::Value << modifier.Radial;
				break;
			case ModifierType::Invert:
				out << YAML::Key << "x" << YAML::Value << modifier.InvertX;
				out << YAML::Key << "y" << YAML::Value << modifier.InvertY;
				out << YAML::Key << "z" << YAML::Value << modifier.InvertZ;
				break;
			case ModifierType::Scale:
				out << YAML::Key << "factor" << YAML::Value << modifier.Scalar.x;
				break;
			case ModifierType::ResponseCurve:
				out << YAML::Key << "exponent" << YAML::Value << modifier.Exponent;
				break;
			case ModifierType::Swizzle:
				out << YAML::Key << "order" << YAML::Value << YAML::Flow << YAML::BeginSeq
					<< modifier.SwizzleOrder[0] << modifier.SwizzleOrder[1] << modifier.SwizzleOrder[2]
					<< YAML::EndSeq << YAML::Block;
				break;
			case ModifierType::Normalize:
				out << YAML::Key << "max" << YAML::Value << modifier.MaxLength;
				break;
			}
			out << YAML::EndMap;
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
						if (!m.Modifiers.empty())
						{
							out << YAML::Key << "modifiers" << YAML::Value << YAML::BeginSeq;
							for (const InputModifier& modifier : m.Modifiers)
								WriteModifier(out, modifier);
							out << YAML::EndSeq;
						}
						if (!m.Triggers.empty())
						{
							out << YAML::Key << "triggers" << YAML::Value << YAML::BeginSeq;
							for (const InputTrigger& trigger : m.Triggers)
								WriteTrigger(out, trigger);
							out << YAML::EndSeq;
						}
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

		// R1:`default: true` 的上下文自动压栈,这是"触发器 / 修改器 / 上下文优先级真正生效"的前提。
		// 状态类上下文的进出仍归游戏代码或脚本(PushContext / PopContext)。
		m_ContextStack.clear();
		for (const InputMappingContext& context : m_Map.Contexts())
			if (context.IsAutoPush())
				PushContext(context, context.GetPriority());

		InvalidateResolve();
		// 映射表换了:动作槽位数、Id 集合都可能变 ⇒ 解算缓存与触发器历史全部作废。
		for (PlayerState& player : m_Players)
		{
			player.Resolved.clear();
			player.PrevDown.clear();
			player.PrevTriggered.clear();
			player.TriggerStates.clear();
			player.ResolvedEpoch = ~0ull;
		}
	}

	void InputService::SetPlayerCount(uint32_t count)
	{
		InvalidateResolve();
		m_Players.resize(count);
	}

	void InputService::Clear()
	{
		InvalidateResolve();
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
		InvalidateResolve();
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
		InvalidateResolve();
		PlayerState* state = GetOrCreatePlayer(player);
		state->NamedGamepadAxes[axis] = value;
		const int index = ParseGamepadAxisIndex(axis);
		if (index >= 0)
			state->Current.SetGamepadAxis(0, index, value);
	}

	void InputService::SetGamepadAxis(uint32_t player, uint32_t axisIndex, float value)
	{
		InvalidateResolve();
		PlayerState* state = GetOrCreatePlayer(player);
		if (axisIndex < 6)
			state->Current.SetGamepadAxis(0, static_cast<int>(axisIndex), value);
	}

	void InputService::EndFrame()
	{
		for (PlayerState& player : m_Players)
		{
			player.Previous = player.Current;
			// R1:解算结果的历史也要滚,否则下一帧的 Pressed/Released 会把上一帧的边沿再报一次
			// (边沿现在建立在**解算后**的 Triggered / Down 上,而不是原始按键状态上)。
			const std::size_t count = player.Resolved.size();
			for (std::size_t slot = 0; slot < count; ++slot)
			{
				player.PrevDown[slot] = player.Resolved[slot].Down ? 1u : 0u;
				player.PrevTriggered[slot] = player.Resolved[slot].Triggered ? 1u : 0u;
			}
			// R2:注入寿命推进(必须在 per-player 循环内)。Fresh 只活一帧,负责产出第一个
			// Pressed 边沿;RemainingFrames 递减到 0 即自动撤销(注入不会永久粘住)。
			// 只推进"已被解算消费过"的注入:注入发生在解算之后的那一帧不能被吃掉。
			for (auto it = player.Injected.begin(); it != player.Injected.end();)
			{
				if (!it->second.Observed)
				{
					++it;
				}
				else
				{
					it->second.Fresh = false;
					if (--it->second.RemainingFrames == 0)
						it = player.Injected.erase(it);
					else
						++it;
				}
			}
		}
		// 边沿历史换了 ⇒ 下一帧必须重算(与 SetKeyState 共用同一个失效入口)。
		InvalidateResolve();
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



	bool InputService::ActionDown(NameId action, uint32_t player) const
	{
		const ActionState* resolved = ResolvedAction(action, player);
		return resolved != nullptr && resolved->Down;
	}

	bool InputService::ActionDown(const std::string& action, uint32_t player) const
	{
		const InputAction* definition = m_Map.FindAction(action);
		return definition != nullptr && ActionDown(definition->Id, player);
	}

	bool InputService::ActionPressed(NameId action, uint32_t player) const
	{
		const ActionState* resolved = ResolvedAction(action, player);
		return resolved != nullptr && resolved->Pressed;
	}

	bool InputService::ActionPressed(const std::string& action, uint32_t player) const
	{
		const InputAction* definition = m_Map.FindAction(action);
		return definition != nullptr && ActionPressed(definition->Id, player);
	}

	bool InputService::ActionReleased(NameId action, uint32_t player) const
	{
		const ActionState* resolved = ResolvedAction(action, player);
		return resolved != nullptr && resolved->Released;
	}

	bool InputService::ActionReleased(const std::string& action, uint32_t player) const
	{
		const InputAction* definition = m_Map.FindAction(action);
		return definition != nullptr && ActionReleased(definition->Id, player);
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
				if (ActionDown(positive->Id, player))
					value += 1.0f;
		}
		else if (!definition->PositiveAction.empty())
		{
			if (const InputAction* positive = m_Map.FindAction(definition->PositiveAction))
				if (ActionDown(positive->Id, player))
					value += 1.0f;
		}

		if (definition->NegativeActionId.IsValid())
		{
			if (const InputAction* negative = m_Map.FindAction(definition->NegativeActionId))
				if (ActionDown(negative->Id, player))
					value -= 1.0f;
		}
		else if (!definition->NegativeAction.empty())
		{
			if (const InputAction* negative = m_Map.FindAction(definition->NegativeAction))
				if (ActionDown(negative->Id, player))
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

	void InputService::EnsurePlayerCaches(const PlayerState& player) const
	{
		const std::size_t count = m_Map.Actions().size();
		if (player.Resolved.size() == count && player.PrevDown.size() == count)
			return;
		player.Resolved.assign(count, ActionState{});
		player.PrevDown.assign(count, 0u);
		player.PrevTriggered.assign(count, 0u);
		player.ResolvedEpoch = ~0ull;
	}

	bool InputService::RawBindingDown(const PlayerState& player, const InputBinding& binding) const
	{
		switch (binding.Device)
		{
		case InputDevice::Key:
			return player.Current.IsKey(binding.Code);
		case InputDevice::Mouse:
			return player.Current.IsMouseButton(binding.Code);
		case InputDevice::Gamepad:
			return player.Current.IsGamepadButton(binding.DeviceIndex, binding.Code);
		default:
			return false;
		}
	}

	std::size_t InputService::ActionSlotOf(NameId action) const
	{
		const std::vector<InputAction>& actions = m_Map.Actions();
		if (actions.empty())
			return 0;
		const InputAction* found = m_Map.FindAction(action);
		if (!found)
			return actions.size();   // 越界值 = "不在映射表里"
		return static_cast<std::size_t>(found - actions.data());
	}

	ActionState InputService::EvaluateActionOnStack(const PlayerState& player, NameId action, float dt) const
	{
		// R2:注入层优先于一切映射 —— 这是"AI/自动化能真正驱动动作"的唯一正确位置
		// (见 PlayerState::Injected 的说明:写原始键位必被宿主轮询覆盖)。
		if (const auto injected = player.Injected.find(action.Value); injected != player.Injected.end())
		{
			ActionState forced;
			if (injected->second.Value > 0.5f)
			{
				forced.Value = glm::vec3(injected->second.Value, 0.0f, 0.0f);
				forced.Down = true;
				forced.Triggered = injected->second.Fresh;
				forced.Phase = injected->second.Fresh ? TriggerPhase::Triggered : TriggerPhase::Ongoing;
			}
			const_cast<ActionInjection&>(injected->second).Observed = true;
			return forced;
		}

		// 触发器状态按动作驻留(与 M2 同口径):同一动作的多层映射共用一份 hold 计时/双击窗口。
		TriggerState& triggerState = player.TriggerStates[action.Value];

		// 把"一条映射"求值成一个 ActionState:原始按下 -> Modifier 链 -> Trigger 链。
		const auto evaluateMapping = [&](const InputBinding& binding,
			const std::vector<InputModifier>& modifiers, const std::vector<InputTrigger>& triggers)
		{
			ActionState result;
			const bool actuated = RawBindingDown(player, binding);
			glm::vec3 value{ actuated ? 1.0f : 0.0f, 0.0f, 0.0f };
			for (const InputModifier& modifier : modifiers)
				value = modifier.Apply(value, dt);

			TriggerPhase phase = TriggerPhase::None;
			if (!triggers.empty())
			{
				for (const InputTrigger& trig : triggers)
				{
					phase = trig.Evaluate(actuated, dt, triggerState);
					if (phase == TriggerPhase::Triggered)
						break;
				}
			}
			else
			{
				// 无触发器 = 直通:按下那一帧 Triggered,之后 Ongoing(与旧 ActionPressed/ActionDown 等价)。
				phase = actuated ? (triggerState.WasActuated ? TriggerPhase::Ongoing : TriggerPhase::Triggered)
					: TriggerPhase::None;
				triggerState.WasActuated = actuated;
			}

			if (phase == TriggerPhase::Triggered || phase == TriggerPhase::Ongoing)
			{
				result.Value = value;
				result.Phase = phase;
				result.Triggered = (phase == TriggerPhase::Triggered);
				result.Down = true;
				result.HeldTime = triggerState.HeldTime;
			}
			return result;
		};

		// 第 1 层:显式上下文栈。PushContext 按 priority **从高到低插入**(降序),所以正序遍历
		// 就是「高优先先算」—— 必须与 PushContext 的插入序一致,不能想当然地反向走。
		for (auto it = m_ContextStack.begin(); it != m_ContextStack.end(); ++it)
		{
			const std::vector<const ActionBindingConfig*> mappings = it->FindMappings(action);
			if (mappings.empty())
				continue;
			for (const ActionBindingConfig* config : mappings)
			{
				const ActionState result = evaluateMapping(config->Binding, config->Modifiers, config->Triggers);
				if (result.Down)
					return result;
			}
			// 本层对本动作有映射但都没触发:声明了消费输入就不再向低优先层传递(与 M2 口径一致)。
			if (it->ConsumesInput())
				return ActionState{};
		}

		// 第 2 层(最低优先):动作自带 bindings。
		// R1 的关键改动就在这两层的关系:旧实现有一个 `m_ContextStack.empty()` 回退分支,
		// 于是"带 trigger 的 contexts"与"裸 bindings"成了两套语义,而文档只描述了前者 ——
		// 结果是触发器/修改器在生产里从不参与解算,文件里写的交互语义全是空的。
		// 现在它们是同一条解算路径上的两个优先级层:任何映射都要过 Modifier 链与 Trigger 链。
		if (const InputAction* definition = m_Map.FindAction(action))
		{
			for (const InputBinding& binding : definition->Bindings)
			{
				const ActionState result = evaluateMapping(binding, {}, {});
				if (result.Down)
					return result;
			}
		}
		return ActionState{};
	}

	void InputService::InjectAction(uint32_t player, NameId action, float value, uint32_t frames)
	{
		PlayerState* state = GetOrCreatePlayer(player);
		ActionInjection injection;
		injection.Value = value;
		injection.RemainingFrames = frames == 0 ? 1u : frames;
		injection.Fresh = true;
		state->Injected[action.Value] = injection;
		InvalidateResolve();
	}

	void InputService::ClearInjection(uint32_t player)
	{
		if (PlayerState* state = GetPlayer(player) ? GetOrCreatePlayer(player) : nullptr)
		{
			state->Injected.clear();
			InvalidateResolve();
		}
	}

	void InputService::ResolvePlayerIfStale(const PlayerState& player) const
	{
		EnsurePlayerCaches(player);
		if (player.ResolvedEpoch == m_ResolveRevision)
			return;
		player.ResolvedEpoch = m_ResolveRevision;

		const std::vector<InputAction>& actions = m_Map.Actions();
		for (std::size_t slot = 0; slot < actions.size(); ++slot)
		{
			ActionState resolved = EvaluateActionOnStack(player, actions[slot].Id, m_FrameDelta);
			// 边沿统一在缓存层算(而不是在触发器里算),这样五类交互的"该响一次"都成立:
			//   Pressed  = Triggered 的上升沿 —— 裸绑定=按下那一帧;Hold=跨过阈值那一帧;
			//              Tap=限时内抬起那一帧;DoubleTap=第二击;Pulse=每个脉冲
			//   Released = Down(激活)的下降沿
			resolved.Pressed = resolved.Triggered && player.PrevTriggered[slot] == 0;
			resolved.Released = !resolved.Down && player.PrevDown[slot] != 0;
			player.Resolved[slot] = resolved;
		}
	}

	const ActionState* InputService::ResolvedAction(NameId action, uint32_t player) const
	{
		const PlayerState* state = GetPlayer(player);
		if (!state)
			return nullptr;
		const std::size_t slot = ActionSlotOf(action);
		if (slot >= m_Map.Actions().size())
			return nullptr;
		ResolvePlayerIfStale(*state);
		if (slot >= state->Resolved.size())
			return nullptr;
		return &state->Resolved[slot];
	}

	ActionState InputService::EvaluateActionState(NameId action, uint32_t player, float dt) const
	{
		// dt 由调用方显式给:这是"可控时间步"的确定性入口(单测/回放用),不读 m_FrameDelta。
		const PlayerState* state = GetPlayer(player);
		if (!state)
			return ActionState{};
		return EvaluateActionOnStack(*state, action, dt);
	}

}
