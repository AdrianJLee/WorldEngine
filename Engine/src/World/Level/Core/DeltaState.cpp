#include "wldpch.h"
#include "World/Level/Core/DeltaState.h"
#include "World/Scene/Components.h"
#include "World/Scene/Entity.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace World::Level
{
	struct DeltaStateRegistry::Impl
	{
		std::unordered_set<uint64_t> DestroyedEntities;
		std::unordered_map<uint64_t, std::unordered_map<std::string, std::string>> PropertyOverrides;
	};

	DeltaStateRegistry::DeltaStateRegistry()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	DeltaStateRegistry::~DeltaStateRegistry() = default;

	void DeltaStateRegistry::MarkDestroyed(uint64_t entityUuid)
	{
		m_Impl->DestroyedEntities.insert(entityUuid);
	}

	bool DeltaStateRegistry::IsDestroyed(uint64_t entityUuid) const
	{
		return m_Impl->DestroyedEntities.find(entityUuid) != m_Impl->DestroyedEntities.end();
	}

	void DeltaStateRegistry::SetProperty(uint64_t entityUuid, const std::string& key, const std::string& val)
	{
		m_Impl->PropertyOverrides[entityUuid][key] = val;
	}

	std::string DeltaStateRegistry::GetProperty(uint64_t entityUuid, const std::string& key, const std::string& fallback) const
	{
		auto entIt = m_Impl->PropertyOverrides.find(entityUuid);
		if (entIt == m_Impl->PropertyOverrides.end())
			return fallback;

		auto propIt = entIt->second.find(key);
		if (propIt == entIt->second.end())
			return fallback;

		return propIt->second;
	}

	bool DeltaStateRegistry::HasProperty(uint64_t entityUuid, const std::string& key) const
	{
		auto entIt = m_Impl->PropertyOverrides.find(entityUuid);
		if (entIt == m_Impl->PropertyOverrides.end())
			return false;

		return entIt->second.find(key) != entIt->second.end();
	}

	void DeltaStateRegistry::Clear()
	{
		m_Impl->DestroyedEntities.clear();
		m_Impl->PropertyOverrides.clear();
	}

	size_t DeltaStateRegistry::DestroyedCount() const
	{
		return m_Impl->DestroyedEntities.size();
	}

	size_t DeltaStateRegistry::PropertyCount() const
	{
		size_t count = 0;
		for (const auto& pair : m_Impl->PropertyOverrides)
			count += pair.second.size();
		return count;
	}

	const std::unordered_set<uint64_t>& DeltaStateRegistry::GetDestroyedEntities() const
	{
		return m_Impl->DestroyedEntities;
	}

	const std::unordered_map<uint64_t, std::unordered_map<std::string, std::string>>& DeltaStateRegistry::GetPropertyOverrides() const
	{
		return m_Impl->PropertyOverrides;
	}

	bool DeltaStateRegistry::SaveToFile(const std::string& filepath) const
	{
		try
		{
			YAML::Emitter out;
			out << YAML::BeginMap;
			out << YAML::Key << "version" << YAML::Value << 1;

			// Destroyed list
			out << YAML::Key << "destroyed" << YAML::Value << YAML::BeginSeq;
			for (uint64_t id : m_Impl->DestroyedEntities)
			{
				out << std::to_string(id);
			}
			out << YAML::EndSeq;

			// Property overrides
			out << YAML::Key << "properties" << YAML::Value << YAML::BeginMap;
			for (const auto& [uuid, props] : m_Impl->PropertyOverrides)
			{
				out << YAML::Key << std::to_string(uuid) << YAML::Value << YAML::BeginMap;
				for (const auto& [k, v] : props)
				{
					out << YAML::Key << k << YAML::Value << v;
				}
				out << YAML::EndMap;
			}
			out << YAML::EndMap;
			out << YAML::EndMap;

			std::ofstream fout(filepath, std::ios::trunc);
			if (!fout.is_open()) return false;
			fout << out.c_str();
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool DeltaStateRegistry::LoadFromFile(const std::string& filepath)
	{
		try
		{
			YAML::Node root = YAML::LoadFile(filepath);
			if (!root || !root.IsMap()) return false;

			Clear();

			if (root["destroyed"] && root["destroyed"].IsSequence())
			{
				for (const auto& item : root["destroyed"])
				{
					uint64_t id = std::stoull(item.as<std::string>());
					m_Impl->DestroyedEntities.insert(id);
				}
			}

			if (root["properties"] && root["properties"].IsMap())
			{
				for (auto it = root["properties"].begin(); it != root["properties"].end(); ++it)
				{
					uint64_t uuid = std::stoull(it->first.as<std::string>());
					if (it->second.IsMap())
					{
						for (auto pIt = it->second.begin(); pIt != it->second.end(); ++pIt)
						{
							m_Impl->PropertyOverrides[uuid][pIt->first.as<std::string>()] = pIt->second.as<std::string>();
						}
					}
				}
			}

			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	void DeltaStateRegistry::ApplyDeltaState(Scene& scene, const DeltaStateRegistry& delta)
	{
		auto& reg = scene.GetRegistry();
		std::vector<entt::entity> toDestroy;

		for (auto entity : reg.view<UUIDComponent>())
		{
			const auto& uuidComp = reg.get<UUIDComponent>(entity);
			uint64_t id = static_cast<uint64_t>(uuidComp.ID);

			if (delta.IsDestroyed(id))
			{
				toDestroy.push_back(entity);
				continue;
			}

			if (delta.HasProperty(id, "Transform.Location"))
			{
				std::string locStr = delta.GetProperty(id, "Transform.Location");
				std::stringstream ss(locStr);
				float x = 0, y = 0, z = 0; char comma;
				if (ss >> x >> comma >> y >> comma >> z)
				{
					if (auto* tr = reg.try_get<TransformComponent>(entity))
					{
						tr->SetLocation(glm::vec3(x, y, z));
					}
				}
			}

			if (delta.HasProperty(id, "Health.CurrentHealth"))
			{
				std::string hpStr = delta.GetProperty(id, "Health.CurrentHealth");
				try
				{
					float hp = std::stof(hpStr);
					if (auto* h = reg.try_get<HealthComponent>(entity))
					{
						h->CurrentHealth = hp;
						if (h->CurrentHealth <= 0.0f) h->IsDead = 1;
					}
				}
				catch (...) {}
			}
		}

		for (auto e : toDestroy)
		{
			Entity ent(&scene, e);
			Entity::DestroyEntity(&scene, ent);
		}
	}
}
