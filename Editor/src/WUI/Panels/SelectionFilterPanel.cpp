#include "wldpch.h"
#include "WUI/Panels/SelectionFilterPanel.h"
#include "World/WUI/WuiWidgets.h"
#include <cstdio>

namespace World
{
	SelectionFilterPanel::SelectionFilterPanel()
	{
		m_Layers = {
			{ "Background", Layer_Background, "2D tilemaps, parallax backgrounds, and skyboxes" },
			{ "Decor",      Layer_Decor,      "Static props, foliage, and decorative environment meshes" },
			{ "Trigger",    Layer_Trigger,    "Trigger volumes, checkpoints, and interaction zones" },
			{ "Collider",   Layer_Collider,   "Rigidbodies, static colliders, and character hulls" },
			{ "Light",      Layer_Light,      "Directional, point, and spot lights" },
			{ "Actor",      Layer_Actor,      "Player, NPCs, dynamic characters, and logic entities" },
			{ "UI",         Layer_UI,         "In-world UI, HUD elements, and canvas nodes" }
		};
	}

	void SelectionFilterPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		const Wui::WuiTheme& theme = host.Theme();
		Wui::WuiId baseId = 0x520000;

		float y = rect.Y + 8.0f;

		// 标题
		Wui::Label(ctx, { rect.X + 12.0f, y }, "Selection Filter", theme.Text, theme.FontSizeTitle);
		y += 28.0f;

		// 快速操作工具栏 (4px 栅格规范排布)
		float btnX = rect.X + 12.0f;
		if (Wui::Button(ctx, baseId++, { btnX, y, 76.0f, 22.0f }, "All Select", theme))
		{
			for (const auto& l : m_Layers)
			{
				m_Filter.SetLayerSelectable(l.Mask, true);
			}
		}
		btnX += 80.0f;

		if (Wui::Button(ctx, baseId++, { btnX, y, 76.0f, 22.0f }, "None Select", theme))
		{
			for (const auto& l : m_Layers)
			{
				m_Filter.SetLayerSelectable(l.Mask, false);
			}
		}
		btnX += 80.0f;

		if (Wui::Button(ctx, baseId++, { btnX, y, 76.0f, 22.0f }, "Unlock All", theme))
		{
			for (const auto& l : m_Layers)
			{
				m_Filter.SetLayerLocked(l.Mask, false);
			}
		}
		btnX += 80.0f;

		if (Wui::Button(ctx, baseId++, { btnX, y, 54.0f, 22.0f }, "Reset", theme))
		{
			m_Filter.Reset();
		}
		y += 30.0f;

		// 表头分割线
		Wui::LineSegment(ctx, { rect.X + 12.0f, y }, { rect.X + rect.W - 12.0f, y }, theme.Border, 1.0f);
		y += 6.0f;

		// 表头列名
		Wui::Label(ctx, { rect.X + 16.0f, y }, "Layer Name", theme.TextMuted, theme.FontSizeSmall);
		Wui::Label(ctx, { rect.X + 140.0f, y }, "Selectable", theme.TextMuted, theme.FontSizeSmall);
		Wui::Label(ctx, { rect.X + 230.0f, y }, "Visible", theme.TextMuted, theme.FontSizeSmall);
		Wui::Label(ctx, { rect.X + 310.0f, y }, "Locked", theme.TextMuted, theme.FontSizeSmall);
		y += 20.0f;

		Wui::LineSegment(ctx, { rect.X + 12.0f, y }, { rect.X + rect.W - 12.0f, y }, theme.Border, 1.0f);
		y += 8.0f;

		int selectableCount = 0;
		int visibleCount = 0;
		int lockedCount = 0;

		// 图层列表项
		for (const auto& layer : m_Layers)
		{
			// 图层名称及悬浮提示
			Wui::WuiRect labelHoverRect { rect.X + 12.0f, y, 120.0f, 22.0f };
			Wui::Label(ctx, { rect.X + 16.0f, y + 2.0f }, layer.Name, theme.Text, theme.FontSizeBody);
			Wui::Tooltip(ctx, labelHoverRect, layer.Description);

			// 可选 (Selectable)
			bool selectable = (m_Filter.GetSelectableMask() & layer.Mask) != 0;
			bool prevSelectable = selectable;
			Wui::WuiRect selectRect { rect.X + 140.0f, y, 70.0f, 22.0f };
			if (Wui::Checkbox(ctx, baseId++, selectRect, "Sel", selectable, theme))
			{
				if (selectable != prevSelectable)
				{
					m_Filter.SetLayerSelectable(layer.Mask, selectable);
				}
			}
			if (selectable) selectableCount++;

			// 可见 (Visible)
			bool visible = m_Filter.IsLayerVisible(layer.Mask);
			bool prevVisible = visible;
			Wui::WuiRect visRect { rect.X + 230.0f, y, 65.0f, 22.0f };
			if (Wui::Checkbox(ctx, baseId++, visRect, "Vis", visible, theme))
			{
				if (visible != prevVisible)
				{
					m_Filter.SetLayerVisible(layer.Mask, visible);
				}
			}
			if (visible) visibleCount++;

			// 锁定 (Locked)
			bool locked = m_Filter.IsLayerLocked(layer.Mask);
			bool prevLocked = locked;
			Wui::WuiRect lockRect { rect.X + 310.0f, y, 65.0f, 22.0f };
			if (Wui::Checkbox(ctx, baseId++, lockRect, "Lock", locked, theme))
			{
				if (locked != prevLocked)
				{
					m_Filter.SetLayerLocked(layer.Mask, locked);
				}
			}
			if (locked) lockedCount++;

			y += 24.0f;
			Wui::LineSegment(ctx, { rect.X + 12.0f, y }, { rect.X + rect.W - 12.0f, y }, theme.Border, 0.5f);
			y += 4.0f;
		}

		y += 10.0f;
		// 底部过滤统计摘要
		char summaryBuf[128];
		std::snprintf(summaryBuf, sizeof(summaryBuf), "Active Selectable: %d/%zu  |  Visible: %d/%zu  |  Locked: %d/%zu",
			selectableCount, m_Layers.size(), visibleCount, m_Layers.size(), lockedCount, m_Layers.size());
		Wui::Label(ctx, { rect.X + 16.0f, y }, summaryBuf, theme.TextMuted, theme.FontSizeSmall);
	}
}
