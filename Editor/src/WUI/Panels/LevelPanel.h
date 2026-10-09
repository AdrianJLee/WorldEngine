#pragma once

#include "WUI/Common/EditorPanel.h"
#include "World/Level/Authoring/LevelCatalog.h"

#include <filesystem>
#include <vector>

namespace World
{
	// 关卡面板 —— 列出内容根下 `levels/*/` 的关卡(Level 2.0 关卡包),一键在编辑器里打开。
	//
	// 关卡集合来自**目录结构**(LevelCatalog::Discover),没有清单文件;新建一个关卡 =
	// 在 `assets/levels/` 下加一个目录,不需要改任何登记表。
	class LevelPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "levels"; }
		const char* Title() const override { return "Levels"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		void EnsureLoaded();

		bool m_Loaded = false;
		std::filesystem::path m_ContentRoot;
		std::vector<Level::LevelInfo> m_Levels;
	};
}
