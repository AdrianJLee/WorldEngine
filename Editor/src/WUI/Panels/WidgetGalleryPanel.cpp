#include "wldpch.h"
#include "WidgetGalleryPanel.h"

#include "World/WUI/WuiWidgets.h"

#include <algorithm>

namespace World
{
	// 组件画廊:每帧按停靠布局给出的 rect 重新 Arrange 保留模式控件并绘制。
	// 交互结果写入 m_LastAction 并 RecordOp,便于在 Operations 面板核对。
	void WidgetGalleryPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		Wui::WuiTheme& theme = host.Theme();

		// ---- 控件实例(首次进入时创建,之后跨帧复用) ----
		if (!m_Button)
		{
			m_ComboOptions = { "Option A", "Option B", "Option C" };

			m_Button = std::make_shared<Wui::WuiButton>();
			m_Button->Label = "Click me";
			m_Button->SetId(Wui::HashId("gallery.button"));
			m_Button->OnClick = [this, &ctx] { m_LastAction = "Button clicked"; ctx.RecordOp("gallery", "click", "Button", "Click me"); };

			m_ButtonDisabled = std::make_shared<Wui::WuiButton>();
			m_ButtonDisabled->Label = "Disabled";
			m_ButtonDisabled->Disabled = true;
			m_ButtonDisabled->SetId(Wui::HashId("gallery.button.disabled"));

			m_IconPlay = std::make_shared<Wui::WuiIconButton>();
			m_IconPlay->SetId(Wui::HashId("gallery.icon.play"));
			m_IconPlay->Tooltip = "Icon button (play)";
			m_IconPlay->OnClick = [this, &ctx] { m_LastAction = "Icon button (play)"; ctx.RecordOp("gallery", "click", "IconButton", "play"); };

			m_IconStop = std::make_shared<Wui::WuiIconButton>();
			m_IconStop->SetId(Wui::HashId("gallery.icon.stop"));
			m_IconStop->Tooltip = "Icon button (stop)";
			m_IconStop->OnClick = [this, &ctx] { m_LastAction = "Icon button (stop)"; ctx.RecordOp("gallery", "click", "IconButton", "stop"); };

			m_Field = std::make_shared<Wui::WuiTextField>();
			m_Field->Buffer = &m_Text;
			m_Field->SetId(Wui::HashId("gallery.field"));
			m_Field->OnCommit = [this, &ctx] { m_LastAction = "TextField committed: " + m_Text; ctx.RecordOp("gallery", "commit", "TextField", m_Text); };
			m_Field->OnCancel = [this] { m_LastAction = "TextField edit cancelled"; };

			m_DragFloatWidget = std::make_shared<Wui::WuiDragFloat>();
			m_DragFloatWidget->Value = &m_DragFloat;
			m_DragFloatWidget->Speed = 0.05f;
			m_DragFloatWidget->SetId(Wui::HashId("gallery.dragfloat"));

			m_DragIntWidget = std::make_shared<Wui::WuiDragInt>();
			m_DragIntWidget->Value = &m_DragInt;
			m_DragIntWidget->SetId(Wui::HashId("gallery.dragint"));

			m_Checkbox = std::make_shared<Wui::WuiCheckbox>();
			m_Checkbox->Label = "Checkbox";
			m_Checkbox->Value = &m_CheckA;
			m_Checkbox->SetId(Wui::HashId("gallery.checkbox"));

			m_ToggleX = std::make_shared<Wui::WuiToggle>();
			m_ToggleX->Label = "Toggle A";
			m_ToggleX->Value = &m_ToggleA;
			m_ToggleX->SetId(Wui::HashId("gallery.toggle.a"));
			m_ToggleX->OnChanged = [this, &ctx]
			{
				m_LastAction = std::string("Toggle A ") + (m_ToggleA ? "on" : "off");
				ctx.RecordOp("gallery", "toggle", "Toggle A", m_ToggleA ? "on" : "off");
			};

			m_ToggleY = std::make_shared<Wui::WuiToggle>();
			m_ToggleY->Label = "Toggle B";
			m_ToggleY->Value = &m_ToggleB;
			m_ToggleY->SetId(Wui::HashId("gallery.toggle.b"));
			m_ToggleY->OnChanged = [this, &ctx]
			{
				m_LastAction = std::string("Toggle B ") + (m_ToggleB ? "on" : "off");
				ctx.RecordOp("gallery", "toggle", "Toggle B", m_ToggleB ? "on" : "off");
			};

			m_SliderX = std::make_shared<Wui::WuiSlider>();
			m_SliderX->Value = &m_SliderA;
			m_SliderX->SetId(Wui::HashId("gallery.slider.a"));

			m_SliderY = std::make_shared<Wui::WuiSlider>();
			m_SliderY->Value = &m_SliderB;
			m_SliderY->SetId(Wui::HashId("gallery.slider.b"));

			m_Combo = std::make_shared<Wui::WuiCombo>();
			m_Combo->Options = &m_ComboOptions;
			m_Combo->Selected = &m_ComboIndex;
			m_Combo->SetId(Wui::HashId("gallery.combo"));

			m_Tabs = std::make_shared<Wui::WuiTabs>();
			m_Tabs->Labels = { "General", "Rendering", "Physics" };
			m_Tabs->Selected = &m_TabIndex;
			m_Tabs->SetId(Wui::HashId("gallery.tabs"));
			m_Tabs->OnChanged = [this, &ctx](int index)
			{
				m_LastAction = "Tab changed: " + std::to_string(index);
				ctx.RecordOp("gallery", "select", "Tabs", std::to_string(index));
			};

			m_Menu = std::make_shared<Wui::WuiMenuButton>();
			m_Menu->Label = "Popup Menu";
			m_Menu->Items = { "Duplicate", "Rename...", "Delete" };
			m_Menu->SetId(Wui::HashId("gallery.menu"));
			m_Menu->OnSelect = [this, &ctx](int index)
			{
				m_LastAction = "Menu item " + std::to_string(index) + " selected";
				ctx.RecordOp("gallery", "select", "Menu", m_Menu->Items[static_cast<size_t>(index)]);
			};

			m_Tooltip = std::make_shared<Wui::WuiTooltip>();
			m_Tooltip->Text = "Tooltip overlay: hover 提示";
			m_Tooltip->SetId(Wui::HashId("gallery.tooltip"));

			for (int i = 0; i < 3; ++i)
			{
				auto item = std::make_shared<Wui::WuiListItem>();
				item->Label = "List item " + std::to_string(i + 1);
				item->SetId(Wui::HashId("gallery.list") + static_cast<Wui::WuiId>(i));
				item->OnSelect = [this, &ctx, i]
				{
					m_ListIndex = i;
					m_LastAction = "List item " + std::to_string(i + 1) + " selected";
					ctx.RecordOp("gallery", "select", "ListItem", std::to_string(i));
				};
				m_ListItems.push_back(item);
			}

			for (int i = 0; i < 2; ++i)
			{
				auto node = std::make_shared<Wui::WuiTreeItem>();
				node->Label = i == 0 ? "Scene (parent)" : "Child entity";
				node->Depth = i;
				node->SetId(Wui::HashId("gallery.tree") + static_cast<Wui::WuiId>(i));
				node->Expanded = i == 0 ? &m_TreeExpanded : &m_TreeChildExpanded;
				node->OnSelect = [this, &ctx, i]
				{
					m_TreeIndex = i;
					m_LastAction = "Tree node " + std::to_string(i) + " selected";
					ctx.RecordOp("gallery", "select", "TreeItem", std::to_string(i));
				};
				m_TreeItems.push_back(node);
			}

			m_Separator = std::make_shared<Wui::WuiSeparator>();
			m_Separator->SetId(Wui::HashId("gallery.separator"));
		}

		// 主题/纹理每帧同步(设备重建后 icon 纹理 id 会变化)。
		m_IconPlay->TextureId = host.GetIconId(0);
		m_IconStop->TextureId = host.GetIconId(1);
		m_IconPlay->Theme = &theme;
		m_IconStop->Theme = &theme;
		m_Field->Theme = &theme;
		m_DragFloatWidget->Theme = &theme;
		m_DragIntWidget->Theme = &theme;
		m_Combo->Theme = &theme;
		m_Tabs->Theme = &theme;
		m_ToggleX->Theme = &theme;
		m_ToggleY->Theme = &theme;
		m_SliderX->Theme = &theme;
		m_SliderY->Theme = &theme;
		m_Menu->Theme = &theme;
		m_Tooltip->Theme = &theme;
		m_Separator->Theme = &theme;
		for (auto& item : m_ListItems)
			item->Theme = &theme;
		for (auto& node : m_TreeItems)
			node->Theme = &theme;

		// ---- 布局常量 ----
		const float rowH = 24.0f;
		const float gap = 6.0f;
		const float sectionGap = 14.0f;
		const float x0 = rect.X + 12.0f;
		const float width = std::max(160.0f, rect.W - 24.0f);

		// 内容高度按各段行数估算,用于滚动范围。
		const float contentHeight =
			8.0f + 26.0f + sectionGap +
			(28.0f + rowH + gap + rowH + gap) +          // 按钮/图标/菜单 + 提示/模态
			(28.0f + rowH + gap + rowH) +                // 文本与数值
			(28.0f + rowH + gap) +                       // 复选与开关
			(28.0f + rowH) +                             // 滑条
			(28.0f + rowH) +                             // 下拉
			(28.0f + 26.0f + rowH) +                     // 标签页
			(28.0f + 22.0f * 3.0f + gap + 22.0f * 2.0f + gap) + // 列表与树
			(28.0f + 120.0f + gap + 10.0f) +             // 滚动区
			40.0f;

		Wui::BeginScrollArea(ctx, rect, contentHeight, m_ScrollY, theme);
		Wui::WuiPaintContext paint(ctx);

		float y = rect.Y + 8.0f - m_ScrollY;
		const auto place = [&paint](const std::shared_ptr<Wui::WuiWidget>& widget, const Wui::WuiRect& widgetRect)
		{
			widget->Arrange(widgetRect);
			widget->Paint(paint);
		};
		const auto section = [&](const char* title)
		{
			Wui::Label(ctx, { x0, y + 4 }, title, theme.Accent, 15.0f);
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { x0, y + 22, width, 1.0f }, theme.Border, 0.0f });
			y += 28.0f;
		};

		// 标题 + 最近一次交互
		Wui::Label(ctx, { x0, y }, "WUI Component Gallery", theme.Text, 16.0f);
		Wui::Label(ctx, { x0 + 220.0f, y + 3 }, "last action: " + m_LastAction, theme.TextMuted, 13.0f);
		y += 26.0f + sectionGap;

		// ---- 按钮 / 图标按钮 / 弹出菜单 ----
		section("Buttons / Icon buttons / Menu");
		place(m_Button, { x0, y, 120, rowH });
		place(m_ButtonDisabled, { x0 + 128, y, 120, rowH });
		place(m_IconPlay, { x0 + 256, y, 28, rowH });
		place(m_IconStop, { x0 + 290, y, 28, rowH });
		place(m_Menu, { x0 + 326, y, 130, rowH });
		y += rowH + gap;

		// ---- 悬浮提示 + 模态触发 ----
		const Wui::WuiRect hoverAnchor { x0, y, 150, rowH };
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, hoverAnchor, ctx.IsHovered(hoverAnchor) ? theme.ButtonHover : theme.ButtonBg, 3.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { hoverAnchor.X + 8, hoverAnchor.Y + 4, 0, 0 }, theme.Text, 0, 1.0f, "Hover for tooltip", 15.0f, false });
		m_Tooltip->Anchor = hoverAnchor;

		const Wui::WuiRect modalButton { x0 + 160, y, 130, rowH };
		if (Wui::Button(ctx, Wui::HashId("gallery.modal.open"), modalButton, "Open Modal", theme))
		{
			ctx.SetModal(Wui::HashId("gallery.modal"));
			ctx.RecordOp("gallery", "open", "Modal", "gallery");
		}
		y += rowH + gap;

		// ---- 文本与数值 ----
		section("Text / Numbers");
		place(m_Field, { x0, y, 240, rowH });
		Wui::Label(ctx, { x0 + 250, y + 5 }, "DragFloat", theme.TextMuted, 13.0f);
		place(m_DragFloatWidget, { x0 + 320, y, 90, rowH });
		Wui::Label(ctx, { x0 + 418, y + 5 }, "DragInt", theme.TextMuted, 13.0f);
		place(m_DragIntWidget, { x0 + 478, y, 80, rowH });
		y += rowH + gap;

		// ---- 复选与开关 ----
		section("Checkbox / Toggle");
		place(m_Checkbox, { x0, y, 140, rowH });
		place(m_ToggleX, { x0 + 150, y, 160, rowH });
		place(m_ToggleY, { x0 + 320, y, 160, rowH });
		y += rowH + gap;

		// ---- 滑条 ----
		section("Slider");
		Wui::Label(ctx, { x0, y + 5 }, "A", theme.TextMuted, 13.0f);
		place(m_SliderX, { x0 + 20, y, 200, rowH });
		Wui::Label(ctx, { x0 + 232, y + 5 }, std::to_string(m_SliderA).substr(0, 4), theme.Text, 13.0f);
		Wui::Label(ctx, { x0 + 292, y + 5 }, "B", theme.TextMuted, 13.0f);
		place(m_SliderY, { x0 + 312, y, 200, rowH });
		Wui::Label(ctx, { x0 + 524, y + 5 }, std::to_string(m_SliderB).substr(0, 4), theme.Text, 13.0f);
		y += rowH + gap;

		// ---- 下拉 ----
		section("Combo");
		place(m_Combo, { x0, y, 180, rowH });
		Wui::Label(ctx, { x0 + 190, y + 5 }, "selected index: " + std::to_string(m_ComboIndex), theme.TextMuted, 13.0f);
		y += rowH + gap;

		// ---- 标签页 ----
		section("Tabs");
		place(m_Tabs, { x0, y, 320, 26.0f });
		y += 26.0f + gap;
		Wui::Label(ctx, { x0, y }, "content of tab " + std::to_string(m_TabIndex), theme.TextMuted, 14.0f);
		y += rowH;

		// ---- 列表与树 ----
		section("List items / Tree items");
		for (size_t i = 0; i < m_ListItems.size(); ++i)
		{
			m_ListItems[i]->Selected = static_cast<int>(i) == m_ListIndex;
			m_ListItems[i]->IconTexture = host.GetIconId(static_cast<int>(i));
			place(m_ListItems[i], { x0, y, width * 0.5f, 22.0f });
			y += 22.0f;
		}
		y += gap;
		for (size_t i = 0; i < m_TreeItems.size(); ++i)
		{
			if (i == 1 && !m_TreeExpanded)
				continue; // 子节点仅随父节点展开
			m_TreeItems[i]->Selected = static_cast<int>(i) == m_TreeIndex;
			place(m_TreeItems[i], { x0, y, width * 0.5f, 22.0f });
			y += 22.0f;
		}
		y += gap;

		// ---- 分隔线 ----
		place(m_Separator, { x0, y, width, 1.0f });
		y += 10.0f;

		// ---- 滚动区演示 ----
		section("Scroll area");
		const Wui::WuiRect inner { x0, y, width * 0.6f, 120.0f };
		float innerScroll = 0.0f;
		Wui::BeginScrollArea(ctx, inner, 12 * 22.0f + 8.0f, innerScroll, theme);
		for (int i = 0; i < 12; ++i)
		{
			const Wui::WuiRect row { inner.X + 4, inner.Y + 4 + i * 22.0f - innerScroll, inner.W - 8, 22.0f };
			if (ctx.IsHovered(row))
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, theme.ButtonHover, 2.0f });
			Wui::Label(ctx, { row.X + 8, row.Y + 3 }, "scroll row " + std::to_string(i + 1), theme.Text, 14.0f);
		}
		Wui::EndScrollArea(ctx);

		Wui::EndScrollArea(ctx);

		// ---- 模态 ----
		Wui::WuiRect modalPanel;
		const Wui::WuiId modalId = Wui::HashId("gallery.modal");
		if (Wui::BeginModal(ctx, modalId, "Gallery Modal", { 340, 150 }, &modalPanel, theme))
		{
			Wui::Label(ctx, { modalPanel.X + 16, modalPanel.Y + 44 }, "Modal 组件:遮罩 + 面板 + 按钮。", theme.Text, 14.0f);
			if (Wui::Button(ctx, Wui::HashId("gallery.modal.ok"), { modalPanel.X + 16, modalPanel.Y + 100, 110, 26 }, "OK", theme))
			{
				m_LastAction = "Modal OK";
				ctx.RecordOp("gallery", "close", "Modal", "ok");
				ctx.ClearModal();
			}
			if (Wui::Button(ctx, Wui::HashId("gallery.modal.cancel"), { modalPanel.X + 138, modalPanel.Y + 100, 110, 26 }, "Cancel", theme))
			{
				m_LastAction = "Modal cancelled";
				ctx.RecordOp("gallery", "close", "Modal", "cancel");
				ctx.ClearModal();
			}
			Wui::EndModal(ctx, modalId);
		}
	}
}
