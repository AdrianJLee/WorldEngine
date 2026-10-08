#include "wldpch.h"
#include "World/Gameplay/Framework/InputRemap.h"
#include "World/Core/StringPool.h"
#include <yaml-cpp/yaml.h>
#include <fstream>

namespace World::Gameplay
{
	InputRemapManager& InputRemapManager::Get()
	{
		static InputRemapManager instance;
		return instance;
	}

	void InputRemapManager::SetOverride(NameId action, std::vector<InputBinding> bindings)
	{
		m_Overrides[action.Value] = std::move(bindings);
	}

	const std::vector<InputBinding>* InputRemapManager::GetOverride(NameId action) const
	{
		auto it = m_Overrides.find(action.Value);
		return it != m_Overrides.end() ? &it->second : nullptr;
	}

	void InputRemapManager::ClearOverride(NameId action)
	{
		m_Overrides.erase(action.Value);
	}

	void InputRemapManager::ClearAllOverrides()
	{
		m_Overrides.clear();
	}

	bool InputRemapManager::HasConflict(InputBinding binding, NameId currentAction, RemapConflict* outConflict) const
	{
		for (const auto& [actionVal, bindings] : m_Overrides)
		{
			if (actionVal == currentAction.Value)
				continue;
			for (const auto& b : bindings)
			{
				if (b.Device == binding.Device && b.Code == binding.Code)
				{
					if (outConflict)
					{
						outConflict->ConflictingAction = NameId{ actionVal };
						outConflict->ActionName = StringPool::Get().NameOf(NameId{ actionVal });
						outConflict->Binding = b;
					}
					return true;
				}
			}
		}
		return false;
	}

	bool InputRemapManager::SaveOverrides(const std::filesystem::path& path, std::string* error) const
	{
		try
		{
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "overrides" << YAML::Value << YAML::BeginSeq;
			for (const auto& [actionVal, bindings] : m_Overrides)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "action" << YAML::Value << StringPool::Get().NameOf(NameId{ actionVal });
				out << YAML::Key << "bindings" << YAML::Value << YAML::BeginSeq;
				for (const auto& b : bindings)
				{
					out << YAML::BeginMap;
					out << YAML::Key << "device" << YAML::Value << static_cast<int>(b.Device);
					out << YAML::Key << "code" << YAML::Value << b.Code;
					out << YAML::EndMap;
				}
				out << YAML::EndSeq << YAML::EndMap;
			}
			out << YAML::EndSeq << YAML::EndMap;

			std::filesystem::create_directories(path.parent_path());
			std::ofstream file(path);
			if (!file)
			{
				if (error) *error = "Failed to open overrides file for write";
				return false;
			}
			file << out.c_str();
			if (error) error->clear();
			return true;
		}
		catch (const std::exception& ex)
		{
			if (error) *error = ex.what();
			return false;
		}
	}

	bool InputRemapManager::LoadOverrides(const std::filesystem::path& path, std::string* error)
	{
		if (!std::filesystem::exists(path))
			return true;

		try
		{
			YAML::Node root = YAML::LoadFile(path.string());
			m_Overrides.clear();
			if (root["overrides"])
			{
				for (const auto& entry : root["overrides"])
				{
					std::string name = entry["action"].as<std::string>();
					NameId actionId = StringPool::Get().InternName(name);
					std::vector<InputBinding> bindings;
					for (const auto& b : entry["bindings"])
					{
						InputBinding binding;
						binding.Device = static_cast<InputDevice>(b["device"].as<int>());
						binding.Code = b["code"].as<int>();
						bindings.push_back(binding);
					}
					m_Overrides[actionId.Value] = std::move(bindings);
				}
			}
			if (error) error->clear();
			return true;
		}
		catch (const std::exception& ex)
		{
			if (error) *error = ex.what();
			return false;
		}
	}
}
