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
		for (const auto& [name, desc] : other.m_Registry)
		{
			if (m_Registry.find(name) != m_Registry.end())
			{
				continue;
			}
			m_Registry[name] = desc;
		}
		for (const auto& [category, names] : other.m_CategoryMap)
		{
			for (const auto& n : names)
			{
				// 注意去重，防止二次合并
				if (std::find(m_CategoryMap[category].begin(), m_CategoryMap[category].end(), n) == m_CategoryMap[category].end())
				{
					m_CategoryMap[category].push_back(n);
				}
			}
		}
	}

}