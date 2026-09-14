#pragma once

#include "EditorPanel.h"

namespace World
{
	// 挂靠槽位:主窗口里专门接收独立窗口的区域。
	// 把独立窗口拖到槽位上方(窗口中心进入槽位)即挂靠:面板回到槽位所在标签组。
	class AttachSlotPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "attach_slot"; }
		const char* Title() const override { return "Attach Slot"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
	};
}
