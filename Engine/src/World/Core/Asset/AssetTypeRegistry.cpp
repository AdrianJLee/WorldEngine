#include "wldpch.h"
#include "World/Core/Asset/AssetTypeRegistry.h"

#include "World/Core/Log.h"

#include <algorithm>

namespace World
{
	AssetTypeRegistry& AssetTypeRegistry::Get()
	{
		static AssetTypeRegistry registry;
		return registry;
	}

	void AssetTypeRegistry::Register(AssetTypeDesc desc)
	{
		if (desc.Id.empty())
		{
			WLD_CORE_WARN("[asset-types] ignored a registration with an empty id (label '{0}')", desc.Label);
			return;
		}
		if (desc.Term.empty())
			desc.Term = desc.Label;
		// 同 Id = 覆盖:替换原有项(而不是并行两条),菜单里一个类型只出现一次。
		for (AssetTypeDesc& existing : m_Types)
		{
			if (existing.Id == desc.Id)
			{
				existing = std::move(desc);
				return;
			}
		}
		m_Types.push_back(std::move(desc));
	}

	bool AssetTypeRegistry::Unregister(const std::string& id)
	{
		const auto it = std::find_if(m_Types.begin(), m_Types.end(),
			[&id](const AssetTypeDesc& desc) { return desc.Id == id; });
		if (it == m_Types.end())
			return false;
		m_Types.erase(it);
		return true;
	}

	void AssetTypeRegistry::Clear()
	{
		m_Types.clear();
	}

	std::vector<AssetTypeDesc> AssetTypeRegistry::Sorted() const
	{
		std::vector<AssetTypeDesc> sorted = m_Types;
		std::stable_sort(sorted.begin(), sorted.end(), [](const AssetTypeDesc& lhs, const AssetTypeDesc& rhs)
			{
				// 文件夹恒第一(与资源管理器一致:先建容器,再建资产)。
				if (lhs.IsFolder != rhs.IsFolder)
					return lhs.IsFolder;
				if (lhs.SortOrder != rhs.SortOrder)
					return lhs.SortOrder < rhs.SortOrder;
				return lhs.Id < rhs.Id;
			});
		return sorted;
	}

	const AssetTypeDesc* AssetTypeRegistry::Find(const std::string& id) const
	{
		for (const AssetTypeDesc& desc : m_Types)
		{
			if (desc.Id == id)
				return &desc;
		}
		return nullptr;
	}
}
