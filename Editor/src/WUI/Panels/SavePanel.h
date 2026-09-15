#pragma once

#include "EditorPanel.h"

namespace World
{
	// P2a W8-3:存档面板 —— 槽位列表 + 保存/读取/删除,并显示读档报告。
	class SavePanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "save"; }
		const char* Title() const override { return "Saves"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		std::string m_Status;
		double m_StatusUntil = 0.0;
	};
}
