#include "wldpch.h"
#include "World/WUI/WuiCommand.h"

namespace World::Wui
{
	void WuiCommandRegistry::Register(WuiCommand command)
	{
		if (m_Commands.find(command.Id) == m_Commands.end())
			m_Order.push_back(command.Id);
		m_Commands[command.Id] = std::move(command);
	}

	void WuiCommandRegistry::Execute(WuiId id)
	{
		auto it = m_Commands.find(id);
		if (it != m_Commands.end() && it->second.Action)
			it->second.Action();
	}

	bool WuiCommandRegistry::HandleKey(uint32_t key, bool ctrl, bool shift)
	{
		for (WuiId id : m_Order)
		{
			const WuiCommand& command = m_Commands[id];
			if (command.Key == key && command.Ctrl == ctrl && command.Shift == shift)
			{
				if (command.Action)
					command.Action();
				return true;
			}
		}
		return false;
	}

	const WuiCommand* WuiCommandRegistry::Find(WuiId id) const
	{
		auto it = m_Commands.find(id);
		return it == m_Commands.end() ? nullptr : &it->second;
	}

	std::vector<WuiCommand> WuiCommandRegistry::All() const
	{
		std::vector<WuiCommand> result;
		result.reserve(m_Order.size());
		for (WuiId id : m_Order)
			result.push_back(m_Commands.at(id));
		return result;
	}
}
