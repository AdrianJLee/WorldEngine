#include "wldpch.h"
#include "World/Gameplay/Framework/InputContext.h"
#include "World/Core/StringPool.h"

namespace World::Gameplay
{
	InputMappingContext::InputMappingContext(const std::string& name, int32_t priority, bool consume)
		: m_Name(name), m_Priority(priority), m_ConsumeInput(consume)
	{
		m_Id = StringPool::Get().InternName(name);
	}

	void InputMappingContext::AddMapping(ActionBindingConfig config)
	{
		if (!config.Action.IsValid() && !config.ActionName.empty())
			config.Action = StringPool::Get().InternName(config.ActionName);
		m_Mappings.push_back(std::move(config));
	}

	std::vector<const ActionBindingConfig*> InputMappingContext::FindMappings(NameId action) const
	{
		std::vector<const ActionBindingConfig*> results;
		for (const auto& mapping : m_Mappings)
		{
			if (mapping.Action == action)
				results.push_back(&mapping);
		}
		return results;
	}
}
