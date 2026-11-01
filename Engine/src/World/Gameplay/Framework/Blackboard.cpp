#include "wldpch.h"
#include "World/Gameplay/Framework/Blackboard.h"
#include "World/Utils/Paths.h"

#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace World::Gameplay
{
	namespace
	{
		std::string KeyTypeToString(BlackboardKeyType type)
		{
			switch (type)
			{
			case BlackboardKeyType::Bool: return "Bool";
			case BlackboardKeyType::Int32: return "Int";
			case BlackboardKeyType::Float: return "Float";
			case BlackboardKeyType::Double: return "Double";
			case BlackboardKeyType::Vector2: return "Vector2";
			case BlackboardKeyType::Vector3: return "Vector3";
			case BlackboardKeyType::Vector4: return "Vector4";
			case BlackboardKeyType::Entity: return "Entity";
			case BlackboardKeyType::String: return "String";
			case BlackboardKeyType::ArrayFloat: return "ArrayFloat";
			case BlackboardKeyType::ArrayInt32: return "ArrayInt";
			case BlackboardKeyType::ArrayVector3: return "ArrayVector3";
			case BlackboardKeyType::ArrayEntity: return "ArrayEntity";
			case BlackboardKeyType::ArrayString: return "ArrayString";
			default: return "Float";
			}
		}

		BlackboardKeyType StringToKeyType(const std::string& str)
		{
			if (str == "Bool") return BlackboardKeyType::Bool;
			if (str == "Int" || str == "Int32") return BlackboardKeyType::Int32;
			if (str == "Float") return BlackboardKeyType::Float;
			if (str == "Double") return BlackboardKeyType::Double;
			if (str == "Vector2" || str == "Vec2") return BlackboardKeyType::Vector2;
			if (str == "Vector3" || str == "Vec3") return BlackboardKeyType::Vector3;
			if (str == "Vector4" || str == "Vec4" || str == "Color" || str == "Quat") return BlackboardKeyType::Vector4;
			if (str == "Entity") return BlackboardKeyType::Entity;
			if (str == "String") return BlackboardKeyType::String;
			if (str == "ArrayFloat" || str == "FloatArray" || str == "Float[]") return BlackboardKeyType::ArrayFloat;
			if (str == "ArrayInt" || str == "ArrayInt32" || str == "IntArray" || str == "Int[]") return BlackboardKeyType::ArrayInt32;
			if (str == "ArrayVector3" || str == "Vector3Array" || str == "Vec3[]" || str == "Waypoints") return BlackboardKeyType::ArrayVector3;
			if (str == "ArrayEntity" || str == "EntityArray" || str == "Entity[]" || str == "PerceivedThreats") return BlackboardKeyType::ArrayEntity;
			if (str == "ArrayString" || str == "StringArray" || str == "String[]") return BlackboardKeyType::ArrayString;
			return BlackboardKeyType::Float;
		}

		bool AreValuesEqual(const BlackboardValue& a, const BlackboardValue& b)
		{
			if (a.index() != b.index())
				return false;
			return a == b;
		}
	}

	uint32_t Blackboard::RegisterKey(const std::string& name, BlackboardKeyType type, const BlackboardValue& defaultValue)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		auto it = m_NameToId.find(name);
		if (it != m_NameToId.end())
			return it->second;

		uint32_t newId = static_cast<uint32_t>(m_Values.size());
		m_NameToId[name] = newId;
		m_IdToName.push_back(name);
		m_Types.push_back(type);

		BlackboardValue initVal = defaultValue;
		if (initVal.index() == 0 && std::holds_alternative<bool>(initVal) && type != BlackboardKeyType::Bool)
		{
			switch (type)
			{
			case BlackboardKeyType::Bool: initVal = false; break;
			case BlackboardKeyType::Int32: initVal = int32_t(0); break;
			case BlackboardKeyType::Float: initVal = 0.0f; break;
			case BlackboardKeyType::Double: initVal = 0.0; break;
			case BlackboardKeyType::Vector2: initVal = glm::vec2(0.0f); break;
			case BlackboardKeyType::Vector3: initVal = glm::vec3(0.0f); break;
			case BlackboardKeyType::Vector4: initVal = glm::vec4(0.0f); break;
			case BlackboardKeyType::Entity: initVal = uint32_t(0); break;
			case BlackboardKeyType::String: initVal = std::string(); break;
			case BlackboardKeyType::ArrayFloat: initVal = std::vector<float>(); break;
			case BlackboardKeyType::ArrayInt32: initVal = std::vector<int32_t>(); break;
			case BlackboardKeyType::ArrayVector3: initVal = std::vector<glm::vec3>(); break;
			case BlackboardKeyType::ArrayEntity: initVal = std::vector<uint32_t>(); break;
			case BlackboardKeyType::ArrayString: initVal = std::vector<std::string>(); break;
			}
		}
		m_Values.push_back(initVal);
		return newId;
	}

	uint32_t Blackboard::GetKeyId(const std::string& name) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		auto it = m_NameToId.find(name);
		return (it != m_NameToId.end()) ? it->second : UINT32_MAX;
	}

	bool Blackboard::HasKey(uint32_t keyId) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		return keyId < m_Values.size();
	}

	bool Blackboard::HasKey(const std::string& name) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		return m_NameToId.find(name) != m_NameToId.end();
	}

	bool Blackboard::RemoveKey(const std::string& name)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		auto it = m_NameToId.find(name);
		if (it == m_NameToId.end())
			return false;

		uint32_t id = it->second;
		m_NameToId.erase(it);
		if (id < m_Values.size())
		{
			m_Values[id] = 0.0f; // 重置为空
		}
		m_Observers.erase(id);
		return true;
	}

	BlackboardKeyType Blackboard::GetKeyType(uint32_t keyId) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		return (keyId < m_Types.size()) ? m_Types[keyId] : BlackboardKeyType::Float;
	}

	const std::string& Blackboard::GetKeyName(uint32_t keyId) const
	{
		static const std::string s_Empty;
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		return (keyId < m_IdToName.size()) ? m_IdToName[keyId] : s_Empty;
	}

	bool Blackboard::SetValue(uint32_t keyId, const BlackboardValue& value)
	{
		std::vector<BlackboardObserverFn> observersToNotify;
		{
			std::lock_guard<std::recursive_mutex> lock(m_Mutex);
			if (keyId >= m_Values.size())
				return false;

			if (AreValuesEqual(m_Values[keyId], value))
				return true; // 无变化，不触发通知

			m_Values[keyId] = value;
			if (keyId < 64)
				m_DirtyMask |= (1ULL << keyId);

			auto it = m_Observers.find(keyId);
			if (it != m_Observers.end())
			{
				for (const auto& entry : it->second)
				{
					if (entry.Callback)
						observersToNotify.push_back(entry.Callback);
				}
			}
		}

		// 在锁外调用观察者回调，防止死锁
		for (const auto& cb : observersToNotify)
		{
			cb(keyId, value);
		}
		return true;
	}

	bool Blackboard::SetValue(const std::string& name, const BlackboardValue& value)
	{
		uint32_t id = GetKeyId(name);
		if (id == UINT32_MAX)
		{
			BlackboardKeyType type = BlackboardKeyType::Float;
			if (std::holds_alternative<bool>(value)) type = BlackboardKeyType::Bool;
			else if (std::holds_alternative<int32_t>(value)) type = BlackboardKeyType::Int32;
			else if (std::holds_alternative<float>(value)) type = BlackboardKeyType::Float;
			else if (std::holds_alternative<double>(value)) type = BlackboardKeyType::Double;
			else if (std::holds_alternative<glm::vec2>(value)) type = BlackboardKeyType::Vector2;
			else if (std::holds_alternative<glm::vec3>(value)) type = BlackboardKeyType::Vector3;
			else if (std::holds_alternative<glm::vec4>(value)) type = BlackboardKeyType::Vector4;
			else if (std::holds_alternative<uint32_t>(value)) type = BlackboardKeyType::Entity;
			else if (std::holds_alternative<std::string>(value)) type = BlackboardKeyType::String;
			else if (std::holds_alternative<std::vector<float>>(value)) type = BlackboardKeyType::ArrayFloat;
			else if (std::holds_alternative<std::vector<int32_t>>(value)) type = BlackboardKeyType::ArrayInt32;
			else if (std::holds_alternative<std::vector<glm::vec3>>(value)) type = BlackboardKeyType::ArrayVector3;
			else if (std::holds_alternative<std::vector<uint32_t>>(value)) type = BlackboardKeyType::ArrayEntity;
			else if (std::holds_alternative<std::vector<std::string>>(value)) type = BlackboardKeyType::ArrayString;

			id = RegisterKey(name, type, value);
			return true;
		}
		return SetValue(id, value);
	}

	const BlackboardValue* Blackboard::GetValue(uint32_t keyId) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		return (keyId < m_Values.size()) ? &m_Values[keyId] : nullptr;
	}

	const BlackboardValue* Blackboard::GetValue(const std::string& name) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		auto it = m_NameToId.find(name);
		return (it != m_NameToId.end()) ? &m_Values[it->second] : nullptr;
	}

	uint32_t Blackboard::AddObserver(uint32_t keyId, BlackboardObserverFn callback)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		uint32_t obsId = m_NextObserverId++;
		m_Observers[keyId].push_back({ obsId, std::move(callback) });
		return obsId;
	}

	void Blackboard::RemoveObserver(uint32_t keyId, uint32_t observerId)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		auto it = m_Observers.find(keyId);
		if (it != m_Observers.end())
		{
			auto& vec = it->second;
			vec.erase(std::remove_if(vec.begin(), vec.end(),
				[observerId](const ObserverEntry& e) { return e.Id == observerId; }), vec.end());
		}
	}

	void Blackboard::ClearObservers()
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		m_Observers.clear();
	}

	std::shared_ptr<Blackboard> BlackboardAsset::Instantiate() const
	{
		auto board = std::make_shared<Blackboard>();
		for (const auto& desc : m_KeyDescs)
		{
			board->RegisterKey(desc.Name, desc.Type, desc.DefaultValue);
		}
		return board;
	}

		bool Blackboard::IsArrayKey(uint32_t keyId) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		if (keyId >= m_Types.size()) return false;
		auto t = m_Types[keyId];
		return t == BlackboardKeyType::ArrayFloat ||
		       t == BlackboardKeyType::ArrayInt32 ||
		       t == BlackboardKeyType::ArrayVector3 ||
		       t == BlackboardKeyType::ArrayEntity ||
		       t == BlackboardKeyType::ArrayString;
	}

	bool Blackboard::IsArrayKey(const std::string& name) const
	{
		return IsArrayKey(GetKeyId(name));
	}

	size_t Blackboard::GetArraySize(uint32_t keyId) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		if (keyId >= m_Values.size()) return 0;
		const auto& val = m_Values[keyId];
		if (std::holds_alternative<std::vector<float>>(val)) return std::get<std::vector<float>>(val).size();
		if (std::holds_alternative<std::vector<int32_t>>(val)) return std::get<std::vector<int32_t>>(val).size();
		if (std::holds_alternative<std::vector<glm::vec3>>(val)) return std::get<std::vector<glm::vec3>>(val).size();
		if (std::holds_alternative<std::vector<uint32_t>>(val)) return std::get<std::vector<uint32_t>>(val).size();
		if (std::holds_alternative<std::vector<std::string>>(val)) return std::get<std::vector<std::string>>(val).size();
		return 0;
	}

	size_t Blackboard::GetArraySize(const std::string& name) const
	{
		return GetArraySize(GetKeyId(name));
	}

	bool Blackboard::ClearArray(uint32_t keyId)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		if (keyId >= m_Values.size()) return false;
		auto& val = m_Values[keyId];
		if (std::holds_alternative<std::vector<float>>(val)) return SetValue(keyId, BlackboardValue(std::vector<float>{}));
		if (std::holds_alternative<std::vector<int32_t>>(val)) return SetValue(keyId, BlackboardValue(std::vector<int32_t>{}));
		if (std::holds_alternative<std::vector<glm::vec3>>(val)) return SetValue(keyId, BlackboardValue(std::vector<glm::vec3>{}));
		if (std::holds_alternative<std::vector<uint32_t>>(val)) return SetValue(keyId, BlackboardValue(std::vector<uint32_t>{}));
		if (std::holds_alternative<std::vector<std::string>>(val)) return SetValue(keyId, BlackboardValue(std::vector<std::string>{}));
		return false;
	}

	bool Blackboard::ClearArray(const std::string& name)
	{
		return ClearArray(GetKeyId(name));
	}

	bool BlackboardAsset::SaveToFile(const std::string& path, std::string* error) const
	{
		try
		{
			std::filesystem::path fsPath(path);
			if (fsPath.has_parent_path())
			{
				std::error_code ec;
				std::filesystem::create_directories(fsPath.parent_path(), ec);
			}

			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "FormatVersion" << YAML::Value << 1;
			out << YAML::Key << "AssetType" << YAML::Value << "Blackboard";
			out << YAML::Key << "Keys" << YAML::Value << YAML::BeginSeq;

			for (const auto& key : m_KeyDescs)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "Name" << YAML::Value << key.Name;
				out << YAML::Key << "Type" << YAML::Value << KeyTypeToString(key.Type);
				out << YAML::Key << "Default";
				if (std::holds_alternative<bool>(key.DefaultValue))
					out << YAML::Value << std::get<bool>(key.DefaultValue);
				else if (std::holds_alternative<int32_t>(key.DefaultValue))
					out << YAML::Value << std::get<int32_t>(key.DefaultValue);
				else if (std::holds_alternative<float>(key.DefaultValue))
					out << YAML::Value << std::get<float>(key.DefaultValue);
				else if (std::holds_alternative<double>(key.DefaultValue))
					out << YAML::Value << std::get<double>(key.DefaultValue);
				else if (std::holds_alternative<glm::vec2>(key.DefaultValue))
				{
					const auto& v = std::get<glm::vec2>(key.DefaultValue);
					out << YAML::Value << YAML::Flow << YAML::BeginSeq << v.x << v.y << YAML::EndSeq;
				}
				else if (std::holds_alternative<glm::vec3>(key.DefaultValue))
				{
					const auto& v = std::get<glm::vec3>(key.DefaultValue);
					out << YAML::Value << YAML::Flow << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
				}
				else if (std::holds_alternative<glm::vec4>(key.DefaultValue))
				{
					const auto& v = std::get<glm::vec4>(key.DefaultValue);
					out << YAML::Value << YAML::Flow << YAML::BeginSeq << v.x << v.y << v.z << v.w << YAML::EndSeq;
				}
				else if (std::holds_alternative<uint32_t>(key.DefaultValue))
					out << YAML::Value << std::get<uint32_t>(key.DefaultValue);
				else if (std::holds_alternative<std::string>(key.DefaultValue))
					out << YAML::Value << std::get<std::string>(key.DefaultValue);
				else if (std::holds_alternative<std::vector<float>>(key.DefaultValue))
				{
					out << YAML::Value << YAML::Flow << YAML::BeginSeq;
					for (float f : std::get<std::vector<float>>(key.DefaultValue)) out << f;
					out << YAML::EndSeq;
				}
				else if (std::holds_alternative<std::vector<int32_t>>(key.DefaultValue))
				{
					out << YAML::Value << YAML::Flow << YAML::BeginSeq;
					for (int32_t i : std::get<std::vector<int32_t>>(key.DefaultValue)) out << i;
					out << YAML::EndSeq;
				}
				else if (std::holds_alternative<std::vector<uint32_t>>(key.DefaultValue))
				{
					out << YAML::Value << YAML::Flow << YAML::BeginSeq;
					for (uint32_t e : std::get<std::vector<uint32_t>>(key.DefaultValue)) out << e;
					out << YAML::EndSeq;
				}
				else if (std::holds_alternative<std::vector<std::string>>(key.DefaultValue))
				{
					out << YAML::Value << YAML::Flow << YAML::BeginSeq;
					for (const auto& s : std::get<std::vector<std::string>>(key.DefaultValue)) out << s;
					out << YAML::EndSeq;
				}
				else if (std::holds_alternative<std::vector<glm::vec3>>(key.DefaultValue))
				{
					out << YAML::Value << YAML::BeginSeq;
					for (const auto& pt : std::get<std::vector<glm::vec3>>(key.DefaultValue))
					{
						out << YAML::Flow << YAML::BeginSeq << pt.x << pt.y << pt.z << YAML::EndSeq;
					}
					out << YAML::EndSeq;
				}
				else
					out << YAML::Value << 0.0f;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
			out << YAML::EndMap;

			std::ofstream fout(path);
			if (!fout.is_open())
			{
				if (error) *error = "Failed to open file: " + path;
				return false;
			}
			fout << out.c_str() << "\n";
			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	bool BlackboardAsset::LoadFromFile(const std::string& path, std::string* error)
	{
		try
		{
			YAML::Node root = YAML::LoadFile(path);
			if (!root || !root.IsMap())
			{
				if (error) *error = "Invalid YAML blackboard format: " + path;
				return false;
			}

			m_KeyDescs.clear();
			if (root["Keys"] && root["Keys"].IsSequence())
			{
				for (const auto& kNode : root["Keys"])
				{
					BlackboardKeyDesc desc;
					if (kNode["Name"]) desc.Name = kNode["Name"].as<std::string>();
					if (kNode["Type"]) desc.Type = StringToKeyType(kNode["Type"].as<std::string>());

					if (kNode["Default"])
					{
						switch (desc.Type)
						{
						case BlackboardKeyType::Bool:
							desc.DefaultValue = kNode["Default"].as<bool>(false);
							break;
						case BlackboardKeyType::Int32:
							desc.DefaultValue = kNode["Default"].as<int32_t>(0);
							break;
						case BlackboardKeyType::Float:
							desc.DefaultValue = kNode["Default"].as<float>(0.0f);
							break;
						case BlackboardKeyType::Double:
							desc.DefaultValue = kNode["Default"].as<double>(0.0);
							break;
						case BlackboardKeyType::Vector2:
							if (kNode["Default"].IsSequence() && kNode["Default"].size() >= 2)
								desc.DefaultValue = glm::vec2(kNode["Default"][0].as<float>(), kNode["Default"][1].as<float>());
							else
								desc.DefaultValue = glm::vec2(0.0f);
							break;
						case BlackboardKeyType::Vector3:
							if (kNode["Default"].IsSequence() && kNode["Default"].size() >= 3)
								desc.DefaultValue = glm::vec3(kNode["Default"][0].as<float>(), kNode["Default"][1].as<float>(), kNode["Default"][2].as<float>());
							else
								desc.DefaultValue = glm::vec3(0.0f);
							break;
						case BlackboardKeyType::Vector4:
							if (kNode["Default"].IsSequence() && kNode["Default"].size() >= 4)
								desc.DefaultValue = glm::vec4(kNode["Default"][0].as<float>(), kNode["Default"][1].as<float>(), kNode["Default"][2].as<float>(), kNode["Default"][3].as<float>());
							else
								desc.DefaultValue = glm::vec4(0.0f);
							break;
						case BlackboardKeyType::Entity:
							desc.DefaultValue = kNode["Default"].as<uint32_t>(0);
							break;
						case BlackboardKeyType::String:
							desc.DefaultValue = kNode["Default"].as<std::string>("");
							break;
						case BlackboardKeyType::ArrayFloat:
							if (kNode["Default"].IsSequence())
							{
								std::vector<float> vec;
								for (const auto& it : kNode["Default"]) vec.push_back(it.as<float>());
								desc.DefaultValue = std::move(vec);
							}
							break;
						case BlackboardKeyType::ArrayInt32:
							if (kNode["Default"].IsSequence())
							{
								std::vector<int32_t> vec;
								for (const auto& it : kNode["Default"]) vec.push_back(it.as<int32_t>());
								desc.DefaultValue = std::move(vec);
							}
							break;
						case BlackboardKeyType::ArrayEntity:
							if (kNode["Default"].IsSequence())
							{
								std::vector<uint32_t> vec;
								for (const auto& it : kNode["Default"]) vec.push_back(it.as<uint32_t>());
								desc.DefaultValue = std::move(vec);
							}
							break;
						case BlackboardKeyType::ArrayString:
							if (kNode["Default"].IsSequence())
							{
								std::vector<std::string> vec;
								for (const auto& it : kNode["Default"]) vec.push_back(it.as<std::string>());
								desc.DefaultValue = std::move(vec);
							}
							break;
						case BlackboardKeyType::ArrayVector3:
							if (kNode["Default"].IsSequence())
							{
								std::vector<glm::vec3> vec;
								for (const auto& it : kNode["Default"])
								{
									if (it.IsSequence() && it.size() >= 3)
										vec.push_back(glm::vec3(it[0].as<float>(), it[1].as<float>(), it[2].as<float>()));
								}
								desc.DefaultValue = std::move(vec);
							}
							break;
						}
					}
					m_KeyDescs.push_back(std::move(desc));
				}
			}
			return true;
		}
		catch (const std::exception& e)
		{
			if (error) *error = e.what();
			return false;
		}
	}

	BlackboardRegistry& BlackboardRegistry::Get()
	{
		static BlackboardRegistry s_Instance;
		return s_Instance;
	}

	std::shared_ptr<Blackboard> BlackboardRegistry::GetOrCreateBlackboard(uint32_t entityId, uint32_t assetId)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		auto it = m_Boards.find(entityId);
		if (it != m_Boards.end() && it->second)
			return it->second;

		auto board = std::make_shared<Blackboard>();
		board->RegisterKey("TargetPos", BlackboardKeyType::Vector3, glm::vec3(0.0f));
		board->RegisterKey("CoverPos", BlackboardKeyType::Vector3, glm::vec3(0.0f));
		board->RegisterKey("TargetEntity", BlackboardKeyType::Entity, uint32_t(0));
		board->RegisterKey("Value", BlackboardKeyType::Float, 0.0f);
		board->RegisterKey("Tag", BlackboardKeyType::Int32, int32_t(0));
		board->RegisterKey("HasThreat", BlackboardKeyType::Bool, false);

		m_Boards[entityId] = board;
		return board;
	}

	std::shared_ptr<Blackboard> BlackboardRegistry::GetBlackboard(uint32_t entityId) const
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		auto it = m_Boards.find(entityId);
		return (it != m_Boards.end()) ? it->second : nullptr;
	}

	void BlackboardRegistry::RemoveBlackboard(uint32_t entityId)
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		m_Boards.erase(entityId);
	}

	void BlackboardRegistry::Clear()
	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);
		m_Boards.clear();
	}
}
