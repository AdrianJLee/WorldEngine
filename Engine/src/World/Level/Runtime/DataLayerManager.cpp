#include "wldpch.h"
#include "World/Level/Runtime/DataLayerManager.h"
#include <algorithm>
#include <vector>

namespace World::Level
{
	struct LayerEntry
	{
		uint32_t Mask{ 1 };
		std::string Name;
		bool IsActive{ true };
	};

	struct DataLayerManager::Impl
	{
		std::vector<LayerEntry> Layers;
	};

	DataLayerManager::DataLayerManager()
		: m_Impl(std::make_unique<Impl>())
	{
	}

	DataLayerManager::~DataLayerManager() = default;

	void DataLayerManager::RegisterLayer(uint32_t mask, const std::string& name, bool defaultActive)
	{
		for (auto& l : m_Impl->Layers)
		{
			if (l.Name == name)
			{
				l.Mask = mask;
				l.IsActive = defaultActive;
				return;
			}
		}
		m_Impl->Layers.push_back({ mask, name, defaultActive });
	}

	void DataLayerManager::SetLayerActive(const std::string& name, bool active)
	{
		for (auto& l : m_Impl->Layers)
		{
			if (l.Name == name)
			{
				l.IsActive = active;
				return;
			}
		}
	}

	bool DataLayerManager::IsLayerActive(const std::string& name) const
	{
		for (const auto& l : m_Impl->Layers)
		{
			if (l.Name == name)
				return l.IsActive;
		}
		return false;
	}

	uint32_t DataLayerManager::GetActiveMask() const
	{
		uint32_t activeMask = 0;
		for (const auto& l : m_Impl->Layers)
		{
			if (l.IsActive)
			{
				activeMask |= l.Mask;
			}
		}
		return activeMask;
	}

	bool DataLayerManager::IsEntityVisible(uint32_t entityMask) const
	{
		if (entityMask == 0)
			return true;
		return (entityMask & GetActiveMask()) != 0;
	}

	size_t DataLayerManager::GetRegisteredLayerCount() const
	{
		return m_Impl->Layers.size();
	}
}
