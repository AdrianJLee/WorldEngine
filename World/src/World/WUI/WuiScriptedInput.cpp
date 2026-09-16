#include "wldpch.h"
#include "WuiScriptedInput.h"

namespace World::Wui
{
	WuiScriptedInput& WuiScriptedInput::Get()
	{
		static WuiScriptedInput instance;
		return instance;
	}

	void WuiScriptedInput::QueueClick(const std::string& windowKey, glm::vec2 position)
	{
		Pending& pending = m_Pending[windowKey];
		pending.Position = position;
		pending.Phase = 0;
		pending.FramesLeft = 2;   // press 帧 + release 帧
	}

	bool WuiScriptedInput::HasPending() const
	{
		for (const auto& entry : m_Pending)
			if (entry.second.FramesLeft > 0)
				return true;
		return false;
	}

	void WuiScriptedInput::Apply(const std::string& windowKey, WuiInputState& input)
	{
		auto entry = m_Pending.find(windowKey);
		if (entry == m_Pending.end() || entry->second.FramesLeft <= 0)
			return;
		Pending& pending = entry->second;
		// 注入期间把键盘焦点交给 WUI,否则菜单/下拉的"点外关闭"逻辑会先把它关掉。
		input.MousePos = pending.Position;
		input.WantKeyboard = true;
		if (pending.Phase == 0)
		{
			input.MouseDown[0] = true;
			input.MouseClicked[0] = true;
		}
		else
		{
			input.MouseDown[0] = false;
			input.MouseReleased[0] = true;
		}
		++pending.Phase;
		--pending.FramesLeft;
		if (pending.FramesLeft <= 0)
			m_Pending.erase(entry);
	}
}
