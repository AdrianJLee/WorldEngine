#include "wldpch.h"
#include "Reflection.h"


namespace World
{
	TypeRegistry& TypeRegistry::Get()
	{
		static TypeRegistry instance;
		return instance;
	}
	void TypeRegistry::MergeFrom(const TypeRegistry& other)
	{
		for (const auto& [typeId, desc] : other.m_Registry)
		{
			if (m_Registry.find(typeId) != m_Registry.end())
			{
				continue;
			}
			m_Registry[typeId] = desc;

			auto& nameIds = m_NameToIds[desc.Name];
			if (std::find(nameIds.begin(), nameIds.end(), typeId) == nameIds.end())
				nameIds.push_back(typeId);
		}
		for (const auto& [category, typeIds] : other.m_CategoryMap)
		{
			for (const auto& typeId : typeIds)
			{
				auto& categoryIds = m_CategoryMap[category];
				if (std::find(categoryIds.begin(), categoryIds.end(), typeId) == categoryIds.end())
					categoryIds.push_back(typeId);
			}
		}
	}

}
