#pragma once

#include "EditorPanel.h"

#include "World/WUI/WuiWidget.h"
#include "World/WUI/Widgets/WuiControls.h"

#include <memory>
#include <string>
#include <vector>

namespace World
{
	// 组件画廊:WUI 组件库的可视验收入口。
	// 所有组件在此可见、可交互;每次交互写入操作日志(Operations 面板可查)。
	class WidgetGalleryPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "gallery"; }
		const char* Title() const override { return "Widget Gallery"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		// 演示状态(跨帧保留,真正状态在面板而不是控件实例里)。
		bool m_ToggleA = true;
		bool m_ToggleB = false;
		bool m_CheckA = true;
		float m_SliderA = 0.35f;
		float m_SliderB = 0.75f;
		float m_DragFloat = 12.5f;
		int64_t m_DragInt = 4;
		std::string m_Text = "Editable text";
		int m_ComboIndex = 1;
		int m_TabIndex = 0;
		int m_ListIndex = 0;
		int m_TreeIndex = 0;
		bool m_TreeExpanded = true;
		bool m_TreeChildExpanded = false;
		std::string m_LastAction = "(none)";
		float m_ScrollY = 0.0f;

		// 控件实例:保留模式,跨帧复用(布局每帧 Arrange)。
		std::shared_ptr<Wui::WuiButton> m_Button;
		std::shared_ptr<Wui::WuiButton> m_ButtonDisabled;
		std::shared_ptr<Wui::WuiIconButton> m_IconPlay;
		std::shared_ptr<Wui::WuiIconButton> m_IconStop;
		std::shared_ptr<Wui::WuiTextField> m_Field;
		std::shared_ptr<Wui::WuiDragFloat> m_DragFloatWidget;
		std::shared_ptr<Wui::WuiDragInt> m_DragIntWidget;
		std::shared_ptr<Wui::WuiCheckbox> m_Checkbox;
		std::shared_ptr<Wui::WuiToggle> m_ToggleX;
		std::shared_ptr<Wui::WuiToggle> m_ToggleY;
		std::shared_ptr<Wui::WuiSlider> m_SliderX;
		std::shared_ptr<Wui::WuiSlider> m_SliderY;
		std::shared_ptr<Wui::WuiCombo> m_Combo;
		std::shared_ptr<Wui::WuiTabs> m_Tabs;
		std::shared_ptr<Wui::WuiMenuButton> m_Menu;
		std::shared_ptr<Wui::WuiTooltip> m_Tooltip;
		std::vector<std::shared_ptr<Wui::WuiListItem>> m_ListItems;
		std::vector<std::shared_ptr<Wui::WuiTreeItem>> m_TreeItems;
		std::shared_ptr<Wui::WuiSeparator> m_Separator;
		std::vector<std::string> m_ComboOptions;
	};
}
