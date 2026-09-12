#pragma once

#include "World/WUI/WuiCore.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace World::Wui
{
	struct WuiCommand
	{
		WuiId Id = 0;
		std::string Name;
		uint32_t Key = 0; // 引擎 KeyCodes;0 = 无快捷键
		bool Ctrl = false;
		bool Shift = false;
		std::function<void()> Action;
	};

	// 菜单动作与快捷键的统一入口。
	class WuiCommandRegistry
	{
	public:
		void Register(WuiCommand command);
		void Execute(WuiId id);
		// 匹配快捷键;命中则执行并返回 true。
		bool HandleKey(uint32_t key, bool ctrl, bool shift);
		const WuiCommand* Find(WuiId id) const;
		std::vector<WuiCommand> All() const;

	private:
		std::unordered_map<WuiId, WuiCommand> m_Commands;
		std::vector<WuiId> m_Order;
	};
}
