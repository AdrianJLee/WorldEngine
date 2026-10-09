#include "wldpch.h"
#include "World/Level/Core/DeltaState.h"
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
		return m_Impl->PropertyOverrides.size();
	}
}
