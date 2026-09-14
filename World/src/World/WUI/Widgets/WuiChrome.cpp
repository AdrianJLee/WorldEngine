#include "wldpch.h"
#include "WuiChrome.h"

#include <algorithm>

namespace World::Wui
{
	namespace
	{
		void PushText(WuiContext& ctx, const glm::vec2& pos, const std::string& text,
			const WuiColor& color, float size, bool bold = false)
		{
			ctx.Commands().push_back({ WuiDrawKind::Text, { pos.x, pos.y, 0, 0 }, color, 0.0f, 1.0f, text, size, bold });
		}
	}

	// ---- 基础表面与高亮 ----

	void PanelBackground(WuiContext& ctx, const WuiRect& rect, const WuiColor& color, float radius)
	{
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, color, radius });
	}

	void BarSurface(WuiContext& ctx, const WuiRect& rect, const WuiColor& fill, const WuiColor& border)
	{
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, fill, 0.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, rect.Y + rect.H - 1.0f, rect.W, 1.0f }, border, 0.0f });
	}

	bool HoverRow(WuiContext& ctx, const WuiRect& rect, bool hovered, bool selected, const WuiTheme& theme,
		float radius)
	{
		if (selected)
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonBg, radius });
		else if (hovered)
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonHover, radius });
		return hovered;
	}

	void HighlightOutline(WuiContext& ctx, const WuiRect& rect, const WuiColor& color, float radius, float thickness)
	{
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, color, radius, thickness });
	}

	void DropZoneOverlay(WuiContext& ctx, const WuiRect& rect, float alpha, float radius)
	{
		ctx.Commands().push_back({ WuiDrawKind::Rect, rect, { 0.30f, 0.50f, 0.90f, alpha }, radius });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, rect, { 0.45f, 0.65f, 1.0f, 1.0f }, radius, 2.0f });
	}

	void SectionHeader(WuiContext& ctx, const WuiRect& rect, const std::string& title, const WuiColor& color,
		const WuiTheme& theme, float fontSize)
	{
		PushText(ctx, { rect.X, rect.Y + 2.0f }, title, color, fontSize);
		ctx.Commands().push_back({ WuiDrawKind::Rect, { rect.X, rect.Y + rect.H - 1.0f, rect.W, 1.0f }, theme.Border, 0.0f });
	}

	// ---- 挂靠标签 ----

	AttachTagResult AttachTag(WuiContext& ctx, const WuiRect& rect, const std::string& title, bool active,
		bool closable, const WuiTheme& theme, float fontSize)
	{
		AttachTagResult result;
		result.Rect = rect;
		result.CloseRect = { rect.X + rect.W - 18.0f, rect.Y + 5.0f, 12.0f, 12.0f };
		result.Hovered = ctx.IsHovered(rect);
		result.CloseHovered = closable && ctx.IsHovered(result.CloseRect);

		// 活动标签优先使用面板底色(与 DockTabBar 一致),悬停只在非活动时给反馈。
		if (active)
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.PanelBg, 2.0f });
		else if (result.Hovered)
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonHover, 2.0f });
		if (result.Hovered)
			ctx.SetCursor(WuiCursor::Hand);

		PushText(ctx, { rect.X + (closable ? 8.0f : 7.0f), rect.Y + 4.0f }, title,
			active ? theme.Text : theme.TextMuted, fontSize);

		if (!closable)
		{
			result.Clicked = ctx.IsClicked(rect);
			return result;
		}

		if (result.CloseHovered)
		{
			ctx.Commands().push_back({ WuiDrawKind::Rect, result.CloseRect, theme.ButtonHover, 2.0f });
			ctx.SetCursor(WuiCursor::Hand);
		}
		PushText(ctx, { result.CloseRect.X + 2.0f, result.CloseRect.Y - 1.0f }, "x", theme.TextMuted, fontSize - 1.0f);
		result.CloseClicked = ctx.IsClicked(result.CloseRect);
		result.Clicked = !result.CloseClicked && ctx.IsClicked(rect);
		return result;
	}

	// ---- 停靠标签栏 ----

	DockTabBarResult DockTabBar(WuiContext& ctx, const WuiRect& area, const std::vector<DockTab>& tabs,
		const WuiTheme& theme, float tabHeight, float maxTabWidth)
	{
		DockTabBarResult result;
		ctx.Commands().push_back({ WuiDrawKind::Rect, { area.X, area.Y, area.W, tabHeight }, theme.PanelHeader, 0.0f });
		result.BarHovered = ctx.IsHovered(area);
		if (tabs.empty())
			return result;

		const float slot = std::max(1.0f, (area.W - 8.0f) / static_cast<float>(tabs.size()));
		const float width = std::min(maxTabWidth, slot);
		float x = area.X + 4.0f;
		for (size_t i = 0; i < tabs.size(); ++i)
		{
			const DockTab& tab = tabs[i];
			const WuiRect tabRect { x, area.Y + 2.0f, width, tabHeight - 2.0f };
			const WuiRect closeRect { tabRect.X + tabRect.W - 18.0f, tabRect.Y + 4.0f, 14.0f, 14.0f };
			const bool hovered = ctx.IsHovered(tabRect);
			if (hovered)
				result.Hovered = static_cast<int>(i);

			if (tab.Active)
				ctx.Commands().push_back({ WuiDrawKind::Rect, tabRect, theme.PanelBg, 2.0f });
			else if (hovered)
				ctx.Commands().push_back({ WuiDrawKind::Rect, tabRect, theme.ButtonHover, 2.0f });
			if (hovered)
				ctx.SetCursor(WuiCursor::Hand);

			PushText(ctx, { tabRect.X + 6.0f, tabRect.Y + 3.0f }, tab.Title, theme.Text, 14.0f, tab.Active);

			if (ctx.IsHovered(closeRect))
			{
				ctx.Commands().push_back({ WuiDrawKind::Rect, closeRect, theme.ButtonHover, 2.0f });
				ctx.SetCursor(WuiCursor::Hand);
			}
			PushText(ctx, { closeRect.X + 3.0f, closeRect.Y - 1.0f }, "x", theme.TextMuted, 13.0f);

			if (ctx.IsClicked(closeRect))
				result.Closed = static_cast<int>(i);
			else if (ctx.IsClicked(tabRect))
				result.Clicked = static_cast<int>(i);
			// 拖动起手:按住标签(非关闭键)即标记按下,由调用方决定阈值与 payload。
			if (ctx.Input().MouseDown[0] && hovered && !ctx.IsHovered(closeRect))
				result.DragStart = static_cast<int>(i);
			x += width;
		}
		return result;
	}

	// ---- 分隔条 ----

	SplitterResult Splitter(WuiContext& ctx, const WuiRect& area, bool row, const WuiTheme& theme,
		bool dragging, float thickness)
	{
		SplitterResult result;
		const WuiRect handle = row
			? WuiRect { area.X, area.Y, thickness, area.H }
			: WuiRect { area.X, area.Y, area.W, thickness };
		result.Hovered = ctx.IsHovered(handle);
		result.Active = dragging;
		if (result.Hovered || dragging)
			ctx.Commands().push_back({ WuiDrawKind::Rect, handle, theme.Border, 0.0f });
		if (result.Hovered)
			ctx.SetCursor(row ? WuiCursor::ResizeEW : WuiCursor::ResizeNS);
		result.Position = row ? ctx.Input().MousePos.x : ctx.Input().MousePos.y;
		return result;
	}

	// ---- 工具栏 ----

	WuiRect Toolbar(WuiContext& ctx, const WuiRect& area, const WuiTheme& theme, float rounding, float alpha)
	{
		WuiColor bg = theme.PanelHeader;
		bg.A = alpha;
		ctx.Commands().push_back({ WuiDrawKind::Rect, area, bg, rounding });
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, area, theme.Border, rounding, 1.0f });
		return { area.X + 4.0f, area.Y + 4.0f, std::max(0.0f, area.W - 8.0f), std::max(0.0f, area.H - 8.0f) };
	}

	bool ToolbarIconButton(WuiContext& ctx, WuiId id, const WuiRect& rect, uint64_t textureId,
		const WuiRect& uv, const std::string& fallbackLabel, const WuiTheme& theme, bool enabled)
	{
		const bool hovered = ctx.IsHovered(rect);
		if (hovered && enabled)
		{
			ctx.Commands().push_back({ WuiDrawKind::Rect, rect, theme.ButtonHover, 3.0f });
			ctx.SetCursor(WuiCursor::Hand);
		}
		if (textureId != 0)
			ctx.Commands().push_back({ WuiDrawKind::Image, rect,
				enabled ? WuiColor { 1, 1, 1, 1 } : WuiColor { 1, 1, 1, 0.4f },
				0.0f, 1.0f, "", 15.0f, false, textureId, uv });
		else
			PushText(ctx, { rect.X + 4.0f, rect.Y + rect.H * 0.5f - 8.0f }, fallbackLabel,
				enabled ? theme.Text : theme.TextMuted, 13.0f);
		(void)id;
		return enabled && ctx.IsClicked(rect);
	}

	void ToolbarSeparator(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme)
	{
		ctx.Commands().push_back({ WuiDrawKind::Rect,
			{ rect.X + rect.W * 0.5f, rect.Y + 4.0f, 1.0f, std::max(0.0f, rect.H - 8.0f) }, theme.Border, 0.0f });
	}

	// ---- 右键菜单 ----

	bool BeginContextMenu(WuiContext& ctx, WuiId id, const glm::vec2& pinnedPos, float width,
		size_t itemCount, WuiRect* panel, const WuiTheme& theme)
	{
		if (!ctx.IsPopupOpen(id))
			return false;
		const float height = static_cast<float>(itemCount) * 22.0f + 8.0f;
		const WuiRect rect { pinnedPos.x, pinnedPos.y, width, height };
		if (panel)
			*panel = rect;
		ctx.PushOverlay();
		DrawPanelSurface(ctx, rect, theme);
		return true;
	}

	void EndContextMenu(WuiContext& ctx, WuiId id, const WuiRect& panel, const WuiTheme& theme)
	{
		(void)theme;
		// 外部点击 / Esc 关闭;位置在打开时已钉住,不会跟随鼠标。
		ctx.ClosePopupsOnOutsideClick({ id }, panel);
		ctx.PopOverlay();
	}

	bool ContextMenuItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		const WuiTheme& theme, bool enabled)
	{
		return MenuItem(ctx, id, rect, label, enabled, theme);
	}

	bool ContextMenuToggleItem(WuiContext& ctx, WuiId id, const WuiRect& rect, const std::string& label,
		bool checked, const WuiTheme& theme, bool enabled)
	{
		return MenuItem(ctx, id, rect, label, checked, enabled, theme);
	}

	void ContextMenuSeparator(WuiContext& ctx, const WuiRect& rect, const WuiTheme& theme)
	{
		ctx.Commands().push_back({ WuiDrawKind::Rect,
			{ rect.X + 6.0f, rect.Y + rect.H * 0.5f, std::max(0.0f, rect.W - 12.0f), 1.0f }, theme.Border, 0.0f });
	}

	// ---- 面包屑 ----

	int Breadcrumb(WuiContext& ctx, const WuiRect& area, const std::string& path, const WuiTheme& theme)
	{
		int clicked = -1;
		float x = area.X + 2.0f;
		size_t start = 0;
		int index = 0;
		const float textW = 7.0f; // 粗略字宽(与后端一致即可,仅用于命中)
		while (start <= path.size())
		{
			const size_t slash = path.find('/', start);
			const std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
			if (!segment.empty())
			{
				const float w = textW * static_cast<float>(segment.size()) + 8.0f;
				const WuiRect item { x, area.Y, w, area.H };
				const bool hovered = ctx.IsHovered(item);
				if (hovered)
				{
					ctx.Commands().push_back({ WuiDrawKind::Rect, item, theme.ButtonHover, 2.0f });
					ctx.SetCursor(WuiCursor::Hand);
				}
				PushText(ctx, { item.X + 4.0f, item.Y + (item.H - 13.0f) * 0.5f }, segment,
					hovered ? theme.Text : theme.TextMuted, 13.0f);
				if (ctx.IsClicked(item))
					clicked = index;
				x += w;
				if (slash != std::string::npos)
				{
					PushText(ctx, { x + 2.0f, area.Y + (area.H - 13.0f) * 0.5f }, "/", theme.TextMuted, 13.0f);
					x += 10.0f;
				}
				++index;
			}
			if (slash == std::string::npos)
				break;
			start = slash + 1;
		}
		return clicked;
	}

	// ---- 搜索框 ----

	bool SearchField(WuiContext& ctx, WuiId id, const WuiRect& rect, std::string& buffer,
		const std::string& placeholder, const WuiTheme& theme)
	{
		// 放大镜简化绘制:圆点 + 手柄。
		const WuiRect glass { rect.X + 6.0f, rect.Y + rect.H * 0.5f - 5.0f, 8.0f, 8.0f };
		ctx.Commands().push_back({ WuiDrawKind::RectOutline, glass, theme.TextMuted, 4.0f, 1.0f });
		ctx.Commands().push_back({ WuiDrawKind::Rect, { glass.X + 7.0f, glass.Y + 7.0f, 4.0f, 1.0f }, theme.TextMuted, 0.0f });

		const WuiRect field { rect.X + 18.0f, rect.Y, std::max(20.0f, rect.W - 38.0f), rect.H };
		const WuiRect clearRect { rect.X + rect.W - 18.0f, rect.Y + rect.H * 0.5f - 7.0f, 14.0f, 14.0f };
		bool submitted = false;
		if (buffer.empty())
		{
			ctx.Commands().push_back({ WuiDrawKind::Rect, field, theme.ButtonBg, 3.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, field, theme.Border, 3.0f, 1.0f });
			PushText(ctx, { field.X + 6.0f, field.Y + (field.H - 13.0f) * 0.5f }, placeholder, theme.TextMuted, 13.0f);
			if (ctx.IsClicked(field))
				ctx.SetFocus(id);
			// 聚焦后进入编辑(空字符串也要能输入):直接转发 TextField。
			if (ctx.Focus() == id)
				submitted = TextField(ctx, id, field, buffer, theme);
		}
		else
		{
			submitted = TextField(ctx, id, field, buffer, theme);
			if (ctx.IsHovered(clearRect))
			{
				ctx.Commands().push_back({ WuiDrawKind::Rect, clearRect, theme.ButtonHover, 2.0f });
				ctx.SetCursor(WuiCursor::Hand);
			}
			PushText(ctx, { clearRect.X + 3.0f, clearRect.Y - 1.0f }, "x", theme.TextMuted, 12.0f);
			if (ctx.IsClicked(clearRect))
			{
				buffer.clear();
				ctx.SetFocus(0);
			}
		}
		return submitted;
	}

	// ---- 列表视图 ----

	ListViewResult ListView(WuiContext& ctx, const WuiRect& area, const std::vector<ListViewItem>& items,
		float rowHeight, float& scrollY, const WuiTheme& theme)
	{
		ListViewResult result;
		const float contentHeight = rowHeight * static_cast<float>(items.size()) + 8.0f;
		result.ItemRects.reserve(items.size());
		BeginScrollArea(ctx, area, contentHeight, scrollY, theme);
		for (size_t i = 0; i < items.size(); ++i)
		{
			const ListViewItem& item = items[i];
			const WuiRect row { area.X + 4.0f, area.Y + 4.0f + rowHeight * static_cast<float>(i) - scrollY,
				std::max(0.0f, area.W - 8.0f), rowHeight };
			result.ItemRects.push_back(row); // 索引对齐:即使不可见也占位
			if (row.Y + row.H < area.Y || row.Y > area.Y + area.H)
				continue; // 视野外:不绘制也不命中
			const bool hovered = !item.Disabled && ctx.IsHovered(row);
			if (hovered)
				result.Hovered = static_cast<int>(i);

			if (item.Selected)
				ctx.Commands().push_back({ WuiDrawKind::Rect, row, theme.PanelBg, 2.0f });
			else if (hovered)
				ctx.Commands().push_back({ WuiDrawKind::Rect, row, theme.ButtonHover, 2.0f });

			float textX = row.X + 6.0f;
			if (item.Icon != 0)
			{
				const float icon = 16.0f;
				ctx.Commands().push_back({ WuiDrawKind::Image,
					{ row.X + 4.0f, row.Y + (row.H - icon) * 0.5f, icon, icon },
					WuiColor { 1, 1, 1, item.Disabled ? 0.4f : 1.0f }, 0.0f, 1.0f, "", 15.0f, false, item.Icon, item.Uv });
				textX = row.X + 24.0f;
			}
			PushText(ctx, { textX, row.Y + (row.H - 14.0f) * 0.5f }, item.Label,
				item.Disabled ? theme.TextMuted : theme.Text, 14.0f, item.Selected);
			if (!item.SubLabel.empty())
				PushText(ctx, { row.X + row.W - 8.0f - 7.0f * static_cast<float>(item.SubLabel.size()),
					row.Y + (row.H - 13.0f) * 0.5f }, item.SubLabel, theme.TextMuted, 13.0f);

			if (item.Disabled)
				continue;
			if (ctx.Input().MouseClicked[1] && hovered)
				result.ContextClicked = static_cast<int>(i);
			if (ctx.IsDoubleClicked(row))
				result.DoubleClicked = static_cast<int>(i);
			else if (ctx.IsClicked(row))
				result.Clicked = static_cast<int>(i);
		}
		EndScrollArea(ctx);
		return result;
	}

	// ---- 网格视图 ----

	GridViewResult GridView(WuiContext& ctx, const WuiRect& area, const std::vector<GridViewItem>& items,
		float cellWidth, float cellHeight, float& scrollY, const WuiTheme& theme)
	{
		GridViewResult result;
		result.ItemRects.reserve(items.size());
		const int columns = std::max(1, static_cast<int>(area.W / std::max(1.0f, cellWidth)));
		const float slotW = area.W / static_cast<float>(columns);
		const int rows = static_cast<int>((items.size() + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
		const float contentHeight = cellHeight * static_cast<float>(rows) + 8.0f;
		BeginScrollArea(ctx, area, contentHeight, scrollY, theme);
		for (size_t i = 0; i < items.size(); ++i)
		{
			const GridViewItem& item = items[i];
			const int column = static_cast<int>(i) % columns;
			const int rowIndex = static_cast<int>(i) / columns;
			const WuiRect cell { area.X + slotW * static_cast<float>(column) + 4.0f,
				area.Y + 4.0f + cellHeight * static_cast<float>(rowIndex) - scrollY,
				std::max(0.0f, slotW - 8.0f), cellHeight - 4.0f };
			result.ItemRects.push_back(cell);
			if (cell.Y + cell.H < area.Y || cell.Y > area.Y + area.H)
				continue;
			const bool hovered = !item.Disabled && ctx.IsHovered(cell);
			if (hovered)
				result.Hovered = static_cast<int>(i);

			if (item.Selected)
				ctx.Commands().push_back({ WuiDrawKind::Rect, cell, theme.PanelBg, 3.0f });
			else if (hovered)
				ctx.Commands().push_back({ WuiDrawKind::Rect, cell, theme.ButtonHover, 3.0f });
			ctx.Commands().push_back({ WuiDrawKind::RectOutline, cell,
				item.Selected ? theme.Accent : theme.Border, 3.0f, 1.0f });

			if (item.Icon != 0)
			{
				const float icon = std::min(cell.W - 16.0f, cell.H - 30.0f);
				if (icon > 4.0f)
					ctx.Commands().push_back({ WuiDrawKind::Image,
						{ cell.X + (cell.W - icon) * 0.5f, cell.Y + 8.0f, icon, icon },
						WuiColor { 1, 1, 1, item.Disabled ? 0.4f : 1.0f }, 0.0f, 1.0f, "", 15.0f, false, item.Icon, item.Uv });
			}
			const float labelW = 7.0f * static_cast<float>(item.Label.size());
			PushText(ctx, { cell.X + std::max(4.0f, (cell.W - labelW) * 0.5f), cell.Y + cell.H - 20.0f },
				item.Label, item.Disabled ? theme.TextMuted : theme.Text, 13.0f, item.Selected);

			if (item.Disabled)
				continue;
			if (ctx.Input().MouseClicked[1] && hovered)
				result.ContextClicked = static_cast<int>(i);
			if (ctx.IsDoubleClicked(cell))
				result.DoubleClicked = static_cast<int>(i);
			else if (ctx.IsClicked(cell))
				result.Clicked = static_cast<int>(i);
		}
		EndScrollArea(ctx);
		return result;
	}

	// ---- 树视图 ----

	TreeViewResult TreeView(WuiContext& ctx, const WuiRect& area, const std::vector<TreeViewItem>& items,
		float rowHeight, float& scrollY, const WuiTheme& theme)
	{
		TreeViewResult result;
		const float contentHeight = rowHeight * static_cast<float>(items.size()) + 8.0f;
		result.ItemRects.reserve(items.size());
		result.ArrowRects.reserve(items.size());
		BeginScrollArea(ctx, area, contentHeight, scrollY, theme);
		for (size_t i = 0; i < items.size(); ++i)
		{
			const TreeViewItem& item = items[i];
			const float indent = 6.0f + static_cast<float>(std::max(0, item.Depth)) * 12.0f;
			const WuiRect row { area.X + 2.0f + indent, area.Y + 4.0f + rowHeight * static_cast<float>(i) - scrollY,
				std::max(0.0f, area.W - 4.0f - indent), rowHeight };
			const WuiRect arrow { row.X, row.Y, item.HasChildren ? 16.0f : 0.0f, rowHeight };
			result.ItemRects.push_back(row);
			result.ArrowRects.push_back(arrow);
			if (row.Y + row.H < area.Y || row.Y > area.Y + area.H)
				continue;

			const bool hovered = !item.Disabled && ctx.IsHovered(row);
			if (item.Selected)
				ctx.Commands().push_back({ WuiDrawKind::Rect, row, theme.ButtonHover, 2.0f });
			else if (hovered)
				ctx.Commands().push_back({ WuiDrawKind::Rect, row, WuiColor { 1, 1, 1, 0.06f }, 2.0f });

			if (item.HasChildren)
			{
				PushText(ctx, { row.X + 2.0f, row.Y + (row.H - 13.0f) * 0.5f },
					item.Expanded ? "[-]" : "[+]", theme.TextMuted, 12.0f);
				if (ctx.IsClicked(arrow))
					result.ClickedArrow = static_cast<int>(i);
			}
			const float labelX = row.X + (item.HasChildren ? 18.0f : 2.0f);
			PushText(ctx, { labelX, row.Y + (row.H - 13.0f) * 0.5f }, item.Label,
				item.Disabled ? theme.TextMuted : theme.Text, 13.0f, item.Selected);

			if (item.Disabled)
				continue;
			const WuiRect labelRect { labelX, row.Y, std::max(0.0f, row.W - (labelX - row.X)), row.H };
			if (ctx.Input().MouseClicked[1] && hovered)
				result.ContextClicked = static_cast<int>(i);
			if (ctx.IsDoubleClicked(labelRect))
				result.DoubleClicked = static_cast<int>(i);
			else if (ctx.IsClicked(labelRect))
				result.Clicked = static_cast<int>(i);
		}
		EndScrollArea(ctx);
		return result;
	}
}
