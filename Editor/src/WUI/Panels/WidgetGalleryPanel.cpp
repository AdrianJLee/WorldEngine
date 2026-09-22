#include "wldpch.h"
#include "WidgetGalleryPanel.h"

#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiLocalization.h"
#include "World/WUI/Widgets/WuiChrome.h"

#include <algorithm>
#include <cmath>

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
			(28.0f + 66.0f) +                            // Chrome:工具栏/面包屑/搜索
			(28.0f + 116.0f + 34.0f) +                   // Chrome:列表/网格/树/标签条/右键菜单
			(28.0f + 26.0f + gap + rowH + gap + rowH + gap + rowH + 18.0f + gap) + // Controls 2
			(28.0f + 24.0f + 22.0f * 4.0f + gap + 22.0f + gap + 84.0f + gap) + // Table / Color / Splitter
			(28.0f + rowH * 3.0f + gap + 120.0f + gap) + // Vector / Empty
			(28.0f + (rowH + gap) * 4.0f) +               // U24 Numeric / Reset
			40.0f;

		Wui::BeginScrollArea(ctx, rect, contentHeight, m_ScrollY, theme);
		Wui::WuiPaintContext paint(ctx);

		float y = rect.Y + 8.0f - m_ScrollY;
		const auto place = [&paint](const std::shared_ptr<Wui::WuiWidget>& widget, const Wui::WuiRect& widgetRect)
		{
			widget->Arrange(widgetRect);
			widget->Paint(paint);
		};
		// title 收 std::string(而不是 const char*):U2C 起段落标题走 Wui::Tr 的返回值。
		const auto section = [&](const std::string& title)
		{
			Wui::SectionHeader(ctx, { x0, y, width, 24.0f }, title, theme.Accent, theme, 15.0f);
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
		Wui::PanelBackground(ctx, hoverAnchor, ctx.IsHovered(hoverAnchor) ? theme.ButtonHover : theme.ButtonBg, 3.0f);
		Wui::Label(ctx, { hoverAnchor.X + 8, hoverAnchor.Y + 4 }, "Hover for tooltip", theme.Text, 15.0f);
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
			Wui::HoverRow(ctx, row, ctx.IsHovered(row), false, theme, 2.0f);
			Wui::Label(ctx, { row.X + 8, row.Y + 3 }, "scroll row " + std::to_string(i + 1), theme.Text, 14.0f);
		}
		Wui::EndScrollArea(ctx);

		// ---- 界面骨架组件(WuiChrome):工具栏 / 面包屑 / 搜索 / 列表 / 网格 / 树 / 标签栏 / 分隔条 ----
		section("Chrome: Toolbar / Breadcrumb / Search");
		{
			const Wui::WuiRect toolbarRect { x0, y, width * 0.5f, 32.0f };
			const Wui::WuiRect toolbarContent = Wui::Toolbar(ctx, toolbarRect, theme);
			for (int i = 0; i < 3; ++i)
			{
				const Wui::WuiRect button { toolbarContent.X + i * 32.0f, toolbarContent.Y, 26.0f, 24.0f };
				if (Wui::ToolbarIconButton(ctx, Wui::HashId("gallery.chrome.tool") + static_cast<Wui::WuiId>(i), button,
					host.GetIconId(i), { 0, 1, 1, -1 }, i == 0 ? "A" : (i == 1 ? "B" : "C"), theme))
					m_LastAction = "Chrome toolbar button " + std::to_string(i);
			}
			if (Wui::Breadcrumb(ctx, { x0 + width * 0.52f, y, width * 0.44f, 24.0f }, "Game/Assets/Scenes", theme) >= 0)
				m_LastAction = "Breadcrumb clicked";
			if (Wui::SearchField(ctx, Wui::HashId("gallery.chrome.search"), { x0, y + 34.0f, 220.0f, 24.0f },
				m_ChromeSearch, "Search assets...", theme))
				m_LastAction = "Search submitted: " + m_ChromeSearch;
			const Wui::SplitterResult split = Wui::Splitter(ctx,
				{ x0 + 240.0f, y + 34.0f, 4.0f, 24.0f }, true, theme, false);
			if (split.Hovered)
				m_LastAction = "Splitter hovered";
			y += 66.0f;
		}

		section("Chrome: ListView / GridView / TreeView / TabBar / ContextMenu");
		{
			std::vector<Wui::ListViewItem> listItems;
			for (int i = 0; i < 4; ++i)
			{
				Wui::ListViewItem item;
				item.Id = Wui::HashId("gallery.chrome.list") + static_cast<Wui::WuiId>(i);
				item.Label = "List row " + std::to_string(i + 1);
				item.SubLabel = i % 2 ? "File" : "Folder";
				item.Icon = host.GetIconId(i);
				item.Uv = { 0, 1, 1, -1 };
				item.Selected = m_ChromeListIndex == i;
				listItems.push_back(item);
			}
			float listScroll = 0.0f;
			const Wui::ListViewResult lv = Wui::ListView(ctx, { x0, y, width * 0.3f, 110.0f }, listItems, 24.0f,
				listScroll, theme);
			if (lv.Clicked >= 0)
			{
				m_ChromeListIndex = lv.Clicked;
				m_LastAction = "ListView clicked " + std::to_string(lv.Clicked);
			}

			std::vector<Wui::GridViewItem> gridItems;
			for (int i = 0; i < 3; ++i)
			{
				Wui::GridViewItem item;
				item.Id = Wui::HashId("gallery.chrome.grid") + static_cast<Wui::WuiId>(i);
				item.Label = "Cell " + std::to_string(i + 1);
				item.Icon = host.GetIconId(i);
				item.Uv = { 0, 1, 1, -1 };
				item.Selected = m_ChromeGridIndex == i;
				gridItems.push_back(item);
			}
			float gridScroll = 0.0f;
			const Wui::GridViewResult gv = Wui::GridView(ctx, { x0 + width * 0.32f, y, width * 0.3f, 110.0f },
				gridItems, 74.0f, 96.0f, gridScroll, theme);
			if (gv.Clicked >= 0)
			{
				m_ChromeGridIndex = gv.Clicked;
				m_LastAction = "GridView clicked " + std::to_string(gv.Clicked);
			}

			std::vector<Wui::TreeViewItem> treeItems;
			for (int i = 0; i < 3; ++i)
			{
				Wui::TreeViewItem item;
				item.Id = Wui::HashId("gallery.chrome.tree") + static_cast<Wui::WuiId>(i);
				item.Label = i == 0 ? "Root" : ("Child " + std::to_string(i));
				item.Depth = i == 0 ? 0 : 1;
				item.HasChildren = i == 0;
				item.Expanded = m_ChromeTreeExpanded;
				item.Selected = m_ChromeTreeIndex == i;
				treeItems.push_back(item);
			}
			float treeScroll = 0.0f;
			const Wui::TreeViewResult tv = Wui::TreeView(ctx, { x0 + width * 0.64f, y, width * 0.34f, 110.0f },
				treeItems, 22.0f, treeScroll, theme);
			if (tv.ClickedArrow == 0)
			{
				m_ChromeTreeExpanded = !m_ChromeTreeExpanded;
				m_LastAction = "TreeView toggled";
			}
			else if (tv.Clicked >= 0)
			{
				m_ChromeTreeIndex = tv.Clicked;
				m_LastAction = "TreeView clicked " + std::to_string(tv.Clicked);
			}
			y += 116.0f;

			std::vector<Wui::DockTab> dockTabs = { { 1, "Tab A", m_ChromeTabIndex == 0 },
				{ 2, "Tab B", m_ChromeTabIndex == 1 }, { 3, "Tab C", m_ChromeTabIndex == 2 } };
			const Wui::DockTabBarResult tabBar = Wui::DockTabBar(ctx, { x0, y, width * 0.5f, 24.0f }, dockTabs, theme);
			if (tabBar.Clicked >= 0)
			{
				m_ChromeTabIndex = tabBar.Clicked;
				m_LastAction = "DockTabBar tab " + std::to_string(tabBar.Clicked);
			}
			if (Wui::Button(ctx, Wui::HashId("gallery.chrome.context.open"), { x0 + width * 0.52f, y, 130.0f, 24.0f },
				"Open context menu", theme))
			{
				m_ChromeMenuPos = ctx.Input().MousePos;
				ctx.OpenPopup(Wui::HashId("gallery.chrome.menu"));
			}
			y += 34.0f;
		}

		// ContextMenu 演示:菜单项点击写入操作日志。
		{
			Wui::WuiRect menuPanel;
			const Wui::WuiId menuId = Wui::HashId("gallery.chrome.menu");
			if (Wui::BeginContextMenu(ctx, menuId, m_ChromeMenuPos, 170.0f, 3, &menuPanel, theme))
			{
				if (Wui::ContextMenuItem(ctx, Wui::HashId("gallery.chrome.menu.a"),
					{ menuPanel.X + 4, menuPanel.Y + 4, menuPanel.W - 8, 22 }, "Action A", theme))
				{
					m_LastAction = "ContextMenu: Action A";
					ctx.CloseAllPopups();
				}
				if (Wui::ContextMenuToggleItem(ctx, Wui::HashId("gallery.chrome.menu.toggle"),
					{ menuPanel.X + 4, menuPanel.Y + 26, menuPanel.W - 8, 22 }, "Toggle item", m_ChromeMenuChecked, theme))
				{
					m_ChromeMenuChecked = !m_ChromeMenuChecked;
					m_LastAction = "ContextMenu: toggle";
				}
				Wui::ContextMenuSeparator(ctx, { menuPanel.X + 4, menuPanel.Y + 48, menuPanel.W - 8, 4 }, theme);
				if (Wui::ContextMenuItem(ctx, Wui::HashId("gallery.chrome.menu.b"),
					{ menuPanel.X + 4, menuPanel.Y + 54, menuPanel.W - 8, 22 }, "Action B", theme))
				{
					m_LastAction = "ContextMenu: Action B";
					ctx.CloseAllPopups();
				}
				Wui::EndContextMenu(ctx, menuId, menuPanel, theme);
			}
		}

		// ---- U2A 新控件:标签条 / 分段按钮 / 混合勾选 / 行内错误 ----
		section("Controls 2");
		{
			int& tabIndex = ctx.Persist<int>(Wui::HashId("gallery.controls2.tab.state"), 0);
			int closeRequest = -1;
			if (Wui::TabBar(ctx, Wui::HashId("gallery.controls2.tabbar"), { x0, y, width * 0.5f, 26.0f },
				{ "General", "Controls 2", "Stats" }, tabIndex, theme, &closeRequest))
				m_LastAction = "TabBar: tab " + std::to_string(tabIndex);
			if (closeRequest >= 0)
				m_LastAction = "TabBar close requested: " + std::to_string(closeRequest);
			y += 26.0f + gap;

			int& segmented = ctx.Persist<int>(Wui::HashId("gallery.controls2.segment.state"), 0);
			if (Wui::Segmented(ctx, Wui::HashId("gallery.controls2.segmented"), { x0, y, width * 0.5f, rowH },
				{ "Left", "Center", "Right" }, segmented, theme))
				m_LastAction = "Segmented: " + std::to_string(segmented);
			y += rowH + gap;

			// 混合 = 管辖的多个对象取值不同;点击后由调用方把 value 统一写给自己管辖的对象。
			bool& mixedValue = ctx.Persist<bool>(Wui::HashId("gallery.controls2.mixed.value"), false);
			bool& mixedFlag = ctx.Persist<bool>(Wui::HashId("gallery.controls2.mixed.flag"), true);
			if (Wui::CheckboxMixed(ctx, Wui::HashId("gallery.controls2.mixed"), { x0, y, 240.0f, rowH },
				"Mixed rows", mixedValue, mixedFlag, theme))
			{
				mixedFlag = false;
				m_LastAction = "CheckboxMixed: all rows selected";
			}
			y += rowH + gap;

			std::string& errorText = ctx.Persist<std::string>(Wui::HashId("gallery.controls2.field"), std::string());
			const std::string error = errorText.empty() ? "Name is required" : std::string();
			Wui::TextFieldEx(ctx, Wui::HashId("gallery.controls2.fieldex"), { x0, y, 260.0f, rowH }, errorText, theme, error);
			Wui::Label(ctx, { x0 + 272.0f, y + 5.0f }, "inline error (clear the text to fix)",
				theme.TextMuted, 13.0f);
			y += rowH + 18.0f + gap;
		}

		// ---- U2B 新控件:表头排序 / 颜色字段 / 分隔条 ----
		section("Table / Color / Splitter");
		{
			// 表头 + 排序:三列小表格(4 行假数据),按当前排序状态重排显示顺序(不做真实比较,
			// 只演示点击表头 → 排序状态变化 → 表格可见变化)。
			int& sortColumn = ctx.Persist<int>(Wui::HashId("gallery.ux2b.table.sortColumn"), 0);
			bool& ascending = ctx.Persist<bool>(Wui::HashId("gallery.ux2b.table.ascending"), true);
			const std::vector<std::string> columns { "Name", "Type", "Size" };
			const std::vector<float> widths { 150.0f, 110.0f, 70.0f };
			const float tableW = 330.0f;
			const float tableRowH = 22.0f;
			const Wui::WuiRect header { x0, y, tableW, 24.0f };
			if (Wui::TableHeader(ctx, Wui::HashId("gallery.ux2b.table"), header, columns, widths,
				sortColumn, ascending, theme))
			{
				m_LastAction = "TableHeader: sort column " + std::to_string(sortColumn)
					+ (ascending ? " asc" : " desc");
			}
			const Wui::WuiRect body { x0, y + header.H, tableW, tableRowH * 4.0f };
			static const char* rows[4][3] = {
				{ "Player", "Mesh", "12 KB" },
				{ "Ground", "Mesh", "48 KB" },
				{ "Sky", "Texture", "256 KB" },
				{ "Camera", "Entity", "-" },
			};
			for (int row = 0; row < 4; ++row)
			{
				int source = (ascending ? row : 3 - row) + sortColumn;
				source %= 4;
				for (size_t col = 0; col < columns.size(); ++col)
				{
					const Wui::WuiRect cell = Wui::TableCell(body, widths, static_cast<size_t>(row), col, tableRowH);
					Wui::Label(ctx, { cell.X + 8.0f, cell.Y + 4.0f }, rows[source][col],
						col == 0 ? theme.Text : theme.TextMuted, 13.0f);
				}
			}
			y += header.H + body.H + gap;

			// 颜色字段:折叠态一块色块 + 色值;点击展开 R/G/B/A 滑杆 + hex 输入。
			glm::vec4& demoColor = ctx.Persist<glm::vec4>(Wui::HashId("gallery.ux2b.color"),
				glm::vec4 { 0.298f, 0.553f, 1.0f, 1.0f });
			if (Wui::ColorField(ctx, Wui::HashId("gallery.ux2b.colorfield"),
				{ x0, y, 200.0f, 22.0f }, demoColor, theme))
				m_LastAction = "ColorField: value changed";
			y += 22.0f + gap;

			// 分隔条:一条横向演示带,左右两块按 value 分宽(竖向条 = 左右拖动,光标 ResizeEW)。
			float& split = ctx.Persist<float>(Wui::HashId("gallery.ux2b.split"), 150.0f);
			const float stripW = std::min(width, 330.0f);
			const float minSplit = 60.0f;
			const float maxSplit = std::max(minSplit + 20.0f, stripW - 90.0f);
			split = std::clamp(split, minSplit, maxSplit);
			const Wui::WuiRect strip { x0, y, stripW, 84.0f };
			const Wui::WuiRect leftBlock { strip.X, strip.Y, split, strip.H };
			const Wui::WuiRect rightBlock { strip.X + split + 6.0f, strip.Y,
				std::max(0.0f, strip.W - split - 6.0f), strip.H };
			Wui::PanelBackground(ctx, leftBlock, theme.ContentBg, theme.Radius);
			Wui::PanelBackground(ctx, rightBlock, theme.ContentBg, theme.Radius);
			Wui::Label(ctx, { leftBlock.X + 8.0f, leftBlock.Y + 8.0f },
				"Left " + std::to_string(static_cast<int>(split)), theme.TextMuted, 12.0f);
			Wui::Label(ctx, { rightBlock.X + 8.0f, rightBlock.Y + 8.0f }, "Right", theme.TextMuted, 12.0f);
			// 分隔条带 6px 宽、摆在边界上;控件内部自己把命中带撑到 6px(视觉线居中)。
			const Wui::WuiRect splitterBand { strip.X + split, strip.Y, 6.0f, strip.H };
			if (Wui::Splitter(ctx, Wui::HashId("gallery.ux2b.splitter"), splitterBand, true,
				split, minSplit, maxSplit, theme))
				m_LastAction = "Splitter: " + std::to_string(static_cast<int>(split));
			y += strip.H + gap;
		}

		// ---- U2C 新控件:向量字段 / 空状态(本段新增文案走 Tr,内联英文默认;
		// 中文键由主 agent 统一并入 Editor/assets/localization/zh-CN.json) ----
		section(Wui::Tr("panel.gallery.section.vector_empty", "Vector / Empty"));
		{
			// 向量字段:横排(宽)与竖排(窄面板,宽 140)。
			glm::vec3& vectorWide = ctx.Persist<glm::vec3>(Wui::HashId("gallery.ux2c.vec.wide"),
				glm::vec3 { 1.0f, 2.5f, 0.0f });
			if (Wui::Vec3Field(ctx, Wui::HashId("gallery.ux2c.vecfield.wide"), { x0, y, 300.0f, rowH },
				vectorWide, 0.05f, -100.0f, 100.0f, theme))
				m_LastAction = "Vec3Field (horizontal) changed";
			Wui::Label(ctx, { x0 + 312.0f, y + 5.0f },
				Wui::Tr("panel.gallery.vec3.horizontal", "horizontal 300px"), theme.TextMuted, 13.0f);

			glm::vec3& vectorNarrow = ctx.Persist<glm::vec3>(Wui::HashId("gallery.ux2c.vec.narrow"),
				glm::vec3 { 0.25f, 1.0f, -3.5f });
			if (Wui::Vec3Field(ctx, Wui::HashId("gallery.ux2c.vecfield.narrow"), { x0 + 430.0f, y, 140.0f, rowH * 3.0f },
				vectorNarrow, 0.05f, -100.0f, 100.0f, theme, 1))
				m_LastAction = "Vec3Field (vertical) changed";
			Wui::Label(ctx, { x0 + 580.0f, y + 5.0f },
				Wui::Tr("panel.gallery.vec3.vertical", "vertical 140px"), theme.TextMuted, 13.0f);
			y += rowH * 3.0f + gap;

			// 空状态:240×120,带 action 按钮(点击写 m_LastAction)。
			const Wui::WuiRect emptyRect { x0, y, 240.0f, 120.0f };
			if (Wui::EmptyState(ctx, emptyRect, "◇",
				Wui::Tr("panel.gallery.empty.title", "No items yet"),
				Wui::Tr("panel.gallery.empty.hint", "Create an item and it will show up in this list."),
				Wui::Tr("panel.gallery.empty.action", "New Item"),
				Wui::HashId("gallery.ux2c.empty.action"), theme))
				m_LastAction = "EmptyState action";
			Wui::Label(ctx, { x0 + 260.0f, y + 8.0f },
				Wui::Tr("panel.gallery.empty.caption", "EmptyState 240x120"), theme.TextMuted, 13.0f);
			y += emptyRect.H + gap;
		}

		// ---- U24 数值控件 / 固定占位恢复默认(控件层验收;分类规则见 WuiWidgets.h) ----
		// 这一段是 we_engine 的控件级验收演示(DragBarFloat / StepperInt / NumberFieldInt /
		// ResetDefaultButton 各一个),不替代材质面板本身的落地(那是 we_editor 的边界)。
		section(Wui::Tr("panel.gallery.section.numeric_reset", "Numeric / Reset"));
		{
			// DragBarFloat = 感知型归一化区间:条体拖动 + 右侧固定宽度值区(可点输入)。
			float& barValue = ctx.Persist<float>(Wui::HashId("gallery.u24.bar.value"), 0.6f);
			const Wui::WuiNumberStyle barStyle;
			if (Wui::DragBarFloat(ctx, Wui::HashId("gallery.u24.bar"), { x0, y, 300.0f, rowH },
				barValue, 0.0f, 1.0f, theme, barStyle))
				m_LastAction = "DragBarFloat: " + std::to_string(barValue);
			Wui::Label(ctx, { x0 + 312.0f, y + 5.0f },
				Wui::Tr("panel.gallery.numeric.bar", "normalized 0..1 (value + input)"),
				theme.TextMuted, 13.0f);
			y += rowH + gap;

			// StepperInt = 小整数(1..16):[−] 值 [+];值区同样可键入。
			int& stepValue = ctx.Persist<int>(Wui::HashId("gallery.u24.step.value"), 4);
			const Wui::WuiNumberStyle stepStyle;
			if (Wui::StepperInt(ctx, Wui::HashId("gallery.u24.step"), { x0, y, 160.0f, rowH },
				stepValue, 1, 16, theme, stepStyle))
				m_LastAction = "StepperInt: " + std::to_string(stepValue);
			Wui::Label(ctx, { x0 + 172.0f, y + 5.0f },
				Wui::Tr("panel.gallery.numeric.stepper", "small integer 1..16"),
				theme.TextMuted, 13.0f);
			y += rowH + gap;

			// NumberFieldInt = 计数/索引/大范围整数:数字输入框 + 单位后缀。
			int64_t& numberValue = ctx.Persist<int64_t>(Wui::HashId("gallery.u24.number.value"), 0);
			Wui::WuiNumberStyle numberStyle;
			numberStyle.Unit = "ch";
			if (Wui::NumberFieldInt(ctx, Wui::HashId("gallery.u24.number"), { x0, y, 160.0f, rowH },
				numberValue, 0, 7, theme, numberStyle))
				m_LastAction = "NumberFieldInt: " + std::to_string(numberValue);
			Wui::Label(ctx, { x0 + 172.0f, y + 5.0f },
				Wui::Tr("panel.gallery.numeric.number", "index / unit suffix"),
				theme.TextMuted, 13.0f);
			y += rowH + gap;

			// 固定占位恢复默认:行内恒定预留 24px 图标位,偏离默认才高亮可点。
			float& resetValue = ctx.Persist<float>(Wui::HashId("gallery.u24.reset.value"), 0.35f);
			constexpr float kResetDefault = 0.35f;
			const bool resetModified = std::fabs(resetValue - kResetDefault) > 1e-4f;
			const Wui::WuiNumberStyle resetStyle;
			Wui::DragBarFloat(ctx, Wui::HashId("gallery.u24.resetbar"), { x0, y, 300.0f, rowH },
				resetValue, 0.0f, 1.0f, theme, resetStyle);
			if (Wui::ResetDefaultButton(ctx, Wui::HashId("gallery.u24.reset"),
				{ x0 + 308.0f, y, 24.0f, rowH }, resetModified, theme, "Reset"))
				resetValue = kResetDefault;
			Wui::Label(ctx, { x0 + 344.0f, y + 5.0f },
				Wui::Tr("panel.gallery.numeric.reset", "fixed-slot restore default"),
				theme.TextMuted, 13.0f);
			y += rowH + gap;
		}

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
