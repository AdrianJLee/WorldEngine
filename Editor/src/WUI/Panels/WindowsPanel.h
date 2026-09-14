#pragma once

#include "EditorPanel.h"

namespace World
{
	// 独立窗口栏:集中管理脱离停靠的独立 OS 窗口(聚焦 / 收回停靠 / 关闭)。
	// 独立窗口与停靠面板是两套组件,本面板充当二者之间的切换入口。
	class WindowsPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "windows"; }
		const char* Title() const override { return "Independent Windows"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
	};
}
