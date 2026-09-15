#pragma once

#include "EditorPanel.h"
#include "World/Gameplay/LevelList.h"

namespace World
{
	// P2a W10-2:关卡面板 —— 列出 levels.welevel 的关卡(2D/3D 基线),一键在编辑器里打开。
	class LevelPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "levels"; }
		const char* Title() const override { return "Levels"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		void EnsureLoaded();

		bool m_Loaded = false;
		std::filesystem::path m_ListPath;
		std::filesystem::path m_ContentRoot;
		std::vector<Gameplay::LevelEntry> m_Entries;
	};
}
