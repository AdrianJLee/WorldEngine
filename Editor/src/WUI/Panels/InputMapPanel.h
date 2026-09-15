#pragma once

#include "EditorPanel.h"
#include "World/Gameplay/InputMap.h"

#include <filesystem>
#include <string>

namespace World
{
	// W7-2(最小可用):输入映射面板。
	// 功能范围:列出动作 -> 点击"Rebind"后按任意键捕获 -> 写回 <内容根>/input.weinput。
	// 有意不做的:动作增删、多玩家分栏、手柄轴编辑(留待手感确认后迭代)。
	class InputMapPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "input"; }
		const char* Title() const override { return "Input Map"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		void EnsureLoaded();
		bool Save();

		Gameplay::InputMap m_Map;
		std::filesystem::path m_Path;
		bool m_Loaded = false;
		std::string m_RebindingAction;
		std::string m_Status;
	};
}
