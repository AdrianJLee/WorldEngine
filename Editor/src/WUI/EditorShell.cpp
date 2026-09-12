#include "wldpch.h"
#include "EditorShell.h"
#include "../EditorLayer.h"

#include "World/Core/Memory/MemoryTracker.h"
#include "World/Core/KeyCodes.h"
#include "World/ImGui/ImGuiDrawLibrary.h"
#include "World/Renderer/Renderer2D.h"
#include "World/Scene/SceneCamera.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiLayoutStore.h"
#include "World/WUI/WuiWidgets.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace World
{
	namespace
	{
		const char* PanelTitle(const std::string& id)
		{
			if (id == "hierarchy") return "Scene Hierarchy";
			if (id == "properties") return "Properties";
			if (id == "content_browser") return "Content Browser";
			if (id == "view") return "View";
			if (id == "stats") return "Stats";
			if (id == "memory") return "Memory Analyzer";
			if (id == "operations") return "Operations";
			return id.c_str();
		}

		std::string ScriptStateName(ScriptInstanceState state)
		{
			switch (state)
			{
				case ScriptInstanceState::Pending: return "Pending";
				case ScriptInstanceState::Creating: return "Creating";
				case ScriptInstanceState::Running: return "Running";
				case ScriptInstanceState::Destroying: return "Destroying";
				case ScriptInstanceState::Stopped: return "Stopped";
				case ScriptInstanceState::Faulted: return "Faulted";
				default: return "?";
			}
		}

		std::string FormatBytes(size_t bytes)
		{
			const char* units[] = { "B", "KB", "MB", "GB", "TB" };
			double value = static_cast<double>(bytes);
			int unit = 0;
			while (value >= 1024 && unit < 4)
			{
				value /= 1024;
				++unit;
			}
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unit]);
			return buffer;
		}

		const char* AllocatorTypeName(AllocatorType type)
		{
			switch (type)
			{
				case AllocatorType::Stack: return "Stack";
				case AllocatorType::Pool: return "Pool";
				case AllocatorType::DualTrack: return "DualTrack";
				case AllocatorType::Linear: return "Linear";
				default: return "Unknown";
			}
		}

		const char* ZoneName(Wui::DropZone zone)
		{
			switch (zone)
			{
				case Wui::DropZone::Center: return "center";
				case Wui::DropZone::Left: return "left";
				case Wui::DropZone::Right: return "right";
				case Wui::DropZone::Top: return "top";
				case Wui::DropZone::Bottom: return "bottom";
				default: return "?";
			}
		}
	}

	EditorShell::EditorShell(EditorLayer& editor)
		: m_Editor(editor), m_LayoutPath(std::string(WLD_EDITOR_DIR) + "wui-layout.json"),
		m_BrowserPath(std::string(WLD_EDITOR_DIR) + "wui-browser.json")
	{
		const std::vector<Wui::PanelId> panels = { "hierarchy", "properties", "content_browser", "view", "stats", "memory", "operations" };
		m_Panels = panels;
		const Wui::DockLayout fallback = Wui::DockLayout::Default(panels);
		std::string error;
		if (!Wui::WuiLayoutStore::Load(m_LayoutPath, fallback, &m_Layout, &error))
			WLD_CORE_WARN("Failed to load WUI layout, using default: {0}", error);

		// 尊重用户关闭的面板:仅当布局文件缺失/损坏时使用默认布局,
		// 不把"已关闭"的面板强制补回。重新显示由 Window 菜单负责。

		m_Browser.Current = m_Browser.Root;
		LoadBrowserState();
	}

	EditorShell::~EditorShell()
	{
		SaveBrowserState();
	}

	void EditorShell::SaveLayout()
	{
		std::string error;
		if (!Wui::WuiLayoutStore::Save(m_LayoutPath, m_Layout, &error))
			WLD_CORE_WARN("Failed to save WUI layout: {0}", error);
	}

	void EditorShell::RestoreLayout(const std::string& json)
	{
		std::string error;
		Wui::DockLayout restored;
		if (Wui::DockLayout::Deserialize(json, &restored, &error))
		{
			m_Layout = std::move(restored);
			SaveLayout();
		}
		else
			WLD_CORE_WARN("Failed to restore dock layout: {0}", error);
	}

	void EditorShell::RecordDockChange(Wui::WuiContext& ctx, const std::string& action, const std::string& target, const std::string& before)
	{
		const std::string after = m_Layout.Serialize();
		ctx.RecordOp("dock", action, target, "");
		ctx.History().Push("Dock " + action + " " + target,
			[this, before] { RestoreLayout(before); },
			[this, after] { RestoreLayout(after); });
		SaveLayout();
	}

	void EditorShell::TogglePanel(Wui::WuiContext& ctx, const std::string& panel)
	{
		const std::string before = m_Layout.Serialize();
		if (m_Layout.Contains(panel))
		{
			if (m_Layout.RemoveTab(panel))
				RecordDockChange(ctx, "hide", panel, before);
		}
		else
		{
			const std::string anchor = m_Layout.FirstPanel();
			if (anchor.empty())
			{
				// 布局为空(所有面板都被关闭):把该面板作为根标签组恢复。
				Wui::DockLayout fresh;
				fresh.Root.Panels.push_back(panel);
				m_Layout = std::move(fresh);
				RecordDockChange(ctx, "show", panel, before);
			}
			else if (m_Layout.AddTab(panel, anchor, Wui::DropZone::Center))
			{
				RecordDockChange(ctx, "show", panel, before);
			}
		}
	}

	void EditorShell::ResetLayout(Wui::WuiContext& ctx)
	{
		const std::string before = m_Layout.Serialize();
		m_Layout = Wui::DockLayout::Default(m_Panels);
		RecordDockChange(ctx, "reset", "", before);
	}

	void EditorShell::OnRender(Wui::WuiContext& ctx)
	{
		m_Ctx = &ctx;
		m_ViewportRect = {};
		ctx.ClearDropTarget();
		const bool undoKey = ctx.Input().Ctrl && !ctx.Input().Shift && ctx.IsKeyPressed(KeyCodes::Z);
		const bool redoKey = ctx.Input().Ctrl && (ctx.IsKeyPressed(KeyCodes::Y) || (ctx.Input().Shift && ctx.IsKeyPressed(KeyCodes::Z)));
		if (undoKey)
		{
			if (ctx.History().Undo())
				ctx.RecordOp("undo", "undo", ctx.History().UndoName(), "");
		}
		else if (redoKey)
		{
			if (ctx.History().Redo())
				ctx.RecordOp("undo", "redo", ctx.History().RedoName(), "");
		}
		const glm::vec2 viewport = ctx.ViewportSize();
		RenderNode(ctx, m_Layout.Root, { 0, 26, viewport.x, viewport.y - 26 });

		std::string payload;
		if (ctx.AcceptDrop(&payload, "panel:"))
		{
			if (payload.rfind("panel:", 0) == 0)
			{
				const std::string panel = payload.substr(6);
				if (!m_DropTargetPanel.empty() && m_Layout.Contains(panel))
				{
					const std::string before = m_Layout.Serialize();
					if (m_Layout.MoveTab(panel, m_DropTargetPanel, m_DropZone))
						RecordDockChange(ctx, "drop", panel + " -> " + m_DropTargetPanel + "/" + ZoneName(m_DropZone), before);
				}
			}
			// 只在落位消费后清空,避免下一帧 AcceptDrop 读取时目标已被清。
			m_DropTargetPanel.clear();
			m_DropZone = Wui::DropZone::Center;
		}

		// 菜单栏最后绘制:其弹出面板需要盖在所有停靠面板之上。
		DrawMenuBar(ctx);

		ImGuiLayer::ApplyImeState(ctx.Focus() == 0 || ctx.IsTextInputActive());

		DrawModals(ctx);

		std::string dragPayload;
		if (ctx.IsDragActive(&dragPayload))
		{
			std::string label = dragPayload;
			if (dragPayload.rfind("panel:", 0) == 0) label = "停靠面板: " + dragPayload.substr(6);
			else if (dragPayload.rfind("file:", 0) == 0) label = "移动文件: " + dragPayload.substr(5);
			ctx.PushOverlay();
			Label(ctx, ctx.Input().MousePos + glm::vec2 { 14, 14 }, label, m_Theme.Text, 13.0f);
			ctx.PopOverlay();
		}

	}

	void EditorShell::RenderNode(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area)
	{
		if (node.IsTabs())
			RenderTabs(ctx, node, area);
		else
			RenderSplit(ctx, node, area);
	}

	void EditorShell::RenderSplit(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area)
	{
		const bool row = node.Direction == Wui::WuiDirection::Row;
		const size_t count = node.Children.size();
		if (count == 0)
			return;
		const float total = row ? area.W : area.H;
		float cursor = 0;
		for (size_t i = 0; i < count; ++i)
		{
			float size = 0;
			if (count == 1) size = total;
			else if (count == 2) size = i == 0 ? total * node.Ratio : total - total * node.Ratio;
			else size = total / static_cast<float>(count);

			Wui::WuiRect childArea = row
				? Wui::WuiRect { area.X + cursor, area.Y, size, area.H }
				: Wui::WuiRect { area.X, area.Y + cursor, area.W, size };
			RenderNode(ctx, node.Children[i], childArea);
			cursor += size;

			if (i + 1 < count)
			{
				const Wui::WuiRect splitter = row
					? Wui::WuiRect { area.X + cursor - 2, area.Y, 4, area.H }
					: Wui::WuiRect { area.X, area.Y + cursor - 2, area.W, 4 };
				if (ctx.IsHovered(splitter) || m_DragSplitNode == &node)
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, splitter, m_Theme.Border, 0.0f });
				if (ctx.IsHovered(splitter))
					ctx.SetCursor(row ? Wui::WuiCursor::ResizeEW : Wui::WuiCursor::ResizeNS);
				if (ctx.IsClicked(splitter))
				{
					m_SplitterDragging = true;
					m_DragSplitNode = &node;
					m_DragSplitRow = row;
					m_SplitterBeforeJson = m_Layout.Serialize();
				}
			}
		}

		if (m_DragSplitNode == &node && m_SplitterDragging)
		{
			const float mouse = row ? ctx.Input().MousePos.x - area.X : ctx.Input().MousePos.y - area.Y;
			node.Ratio = std::max(0.05f, std::min(0.95f, mouse / std::max(1.0f, total)));
			if (ctx.Input().MouseReleased[0])
			{
				m_SplitterDragging = false;
				m_DragSplitNode = nullptr;
				RecordDockChange(ctx, "resize", "", m_SplitterBeforeJson);
			}
		}
	}

	void EditorShell::RenderTabs(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area)
	{
		const float tabH = 24;
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { area.X, area.Y, area.W, tabH }, m_Theme.PanelHeader, 0.0f });
		float x = area.X + 4;
		for (size_t i = 0; i < node.Panels.size(); ++i)
		{
			const std::string& panel = node.Panels[i];
			const float width = std::min(150.0f, area.W / std::max<size_t>(1, node.Panels.size()));
			const Wui::WuiRect tab { x, area.Y + 2, width, tabH - 2 };
			if (i == node.Active)
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, tab, m_Theme.PanelBg, 2.0f });
			else if (ctx.IsHovered(tab))
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, tab, m_Theme.ButtonHover, 2.0f });
			if (ctx.IsClicked(tab))
				m_Layout.Activate(panel);
			if (ctx.IsHovered(tab))
				ctx.SetCursor(Wui::WuiCursor::Hand);
			if (ctx.Input().MouseDown[0] && ctx.IsHovered(tab))
				ctx.BeginDrag(Wui::HashId(("tab." + panel).c_str()), "panel:" + panel);

			ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { tab.X + 6, tab.Y + 3, 0, 0 }, m_Theme.Text, 0, 1.0f, PanelTitle(panel), 14.0f, false });
			const Wui::WuiRect close { tab.X + tab.W - 18, tab.Y + 4, 14, 14 };
			if (ctx.IsHovered(close))
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, close, m_Theme.ButtonHover, 2.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { close.X + 3, close.Y - 1, 0, 0 }, m_Theme.TextMuted, 0, 1.0f, "x", 13.0f, false });
			if (ctx.IsClicked(close))
			{
				const std::string before = m_Layout.Serialize();
				if (m_Layout.RemoveTab(panel))
					RecordDockChange(ctx, "close", panel, before);
			}
			x += width;
		}

		const Wui::WuiRect content { area.X, area.Y + tabH, area.W, area.H - tabH };
		if (!node.Panels.empty())
			RenderPanelContent(ctx, node.Panels[node.Active], content);

		std::string dragPayload;
		if (ctx.IsDragActive(&dragPayload) && dragPayload.rfind("panel:", 0) == 0 && ctx.IsHovered(area))
		{
			ctx.DropTarget(area, "panel:"); // 武装落点:仅面板拖拽在此生效
			const glm::vec2 rel = ctx.Input().MousePos - glm::vec2 { area.X, area.Y };
			const float lx = area.W > 0 ? rel.x / area.W : 0;
			const float ly = area.H > 0 ? rel.y / area.H : 0;
			if (lx < 0.25f) m_DropZone = Wui::DropZone::Left;
			else if (lx > 0.75f) m_DropZone = Wui::DropZone::Right;
			else if (ly < 0.25f) m_DropZone = Wui::DropZone::Top;
			else if (ly > 0.75f) m_DropZone = Wui::DropZone::Bottom;
			else m_DropZone = Wui::DropZone::Center;
			Wui::WuiRect zone = area;
			if (m_DropZone == Wui::DropZone::Left) zone.W = area.W * 0.25f;
			else if (m_DropZone == Wui::DropZone::Right) { zone.X = area.X + area.W * 0.75f; zone.W = area.W * 0.25f; }
			else if (m_DropZone == Wui::DropZone::Top) zone.H = area.H * 0.25f;
			else if (m_DropZone == Wui::DropZone::Bottom) { zone.Y = area.Y + area.H * 0.75f; zone.H = area.H * 0.25f; }
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, zone, { 0.3f, 0.5f, 0.9f, 0.28f }, 3.0f });
			if (!node.Panels.empty())
			{
				const std::string target = node.Panels[node.Active];
				if (target != m_LastDragTarget || m_DropZone != m_LastDragZone)
					ctx.RecordOp("drag", "hover", target, ZoneName(m_DropZone));
				m_LastDragTarget = target;
				m_LastDragZone = m_DropZone;
				m_DropTargetPanel = node.Panels[node.Active];
			}
		}
	}

	void EditorShell::RenderPanelContent(Wui::WuiContext& ctx, const std::string& id, const Wui::WuiRect& rect)
	{
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, m_Theme.PanelBg, 0.0f });
		if (id == "hierarchy") DrawHierarchy(ctx, rect);
		else if (id == "properties") DrawProperties(ctx, rect);
		else if (id == "content_browser") DrawContentBrowser(ctx, rect);
		else if (id == "view") DrawViewport(ctx, rect);
		else if (id == "stats") DrawStats(ctx, rect);
		else if (id == "memory") DrawMemory(ctx, rect);
		else if (id == "operations") DrawOperations(ctx, rect);
		else Label(ctx, { rect.X + 8, rect.Y + 8 }, PanelTitle(id), m_Theme.TextMuted, 14.0f);
	}

	void EditorShell::DrawMenuBar(Wui::WuiContext& ctx)
	{
		const glm::vec2 viewport = ctx.ViewportSize();
		BeginMenuBar(ctx, { 0, 0, viewport.x, 26 }, m_Theme);
		float x = 8;

		struct MenuEntry { std::string Label; bool Checked; std::function<void()> Action; };
		auto drawMenu = [&](const std::string& menuIdName, const char* title, const std::vector<MenuEntry>& entries)
		{
			const Wui::WuiId menuId = Wui::HashId(menuIdName.c_str());
			const Wui::WuiRect header { x, 2, static_cast<float>(std::strlen(title) * 9 + 22), 22 };
			if (BeginMenu(ctx, menuId, header, title, m_Theme))
			{
				ctx.PushOverlay();
				const Wui::WuiRect panel { header.X, 24, 240, static_cast<float>(entries.size() * 22 + 8) };
				DrawPanelSurface(ctx, panel, m_Theme);
				for (size_t i = 0; i < entries.size(); ++i)
				{
					const Wui::WuiRect item { panel.X + 4, panel.Y + 4 + i * 22, panel.W - 8, 22 };
					const Wui::WuiId itemId = Wui::HashId((menuIdName + "." + entries[i].Label).c_str());
					if (MenuItem(ctx, itemId, item, entries[i].Label, entries[i].Checked, true, m_Theme))
					{
						entries[i].Action();
						ctx.RecordOp("menu", "item", entries[i].Label, menuIdName);
						ctx.CloseAllPopups();
					}
				}
				EndMenu(ctx, menuId, panel, m_Theme);
				ctx.PopOverlay();
			}
			x += header.W + 4;
		};

		drawMenu("menu.file", "File", {
			{ "New", false, [this] { m_Editor.NewScene(); } },
			{ "Open", false, [this] { m_Editor.OpenScene(); } },
			{ "Save", false, [this] { m_Editor.SaveScene(); } },
			{ "Generate Lua API Stubs", false, [this] { m_Editor.GenerateLuaStubsAction(); } },
			{ "Cooking", false, [this] { m_Editor.StartCookingAction(); } },
			{ "Export Operation Log", false, [this] { m_Editor.ExportOperationLog(); } },
			{ "Exit", false, [this] { m_Editor.CloseAction(); } },
		});

		std::vector<MenuEntry> windowEntries;
		for (const std::string& panel : m_Panels)
		{
			const bool visible = m_Layout.Contains(panel);
			windowEntries.push_back({ PanelTitle(panel), visible, [this, panel, &ctx] { TogglePanel(ctx, panel); } });
		}
		windowEntries.push_back({ "Reset Layout", false, [this, &ctx] { ResetLayout(ctx); } });
		drawMenu("menu.window", "Window", windowEntries);

		EndMenuBar(ctx);
	}

	void EditorShell::DrawStats(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		float y = rect.Y + 8;
		const auto& stats = Renderer2D::GetStats();
		Label(ctx, { rect.X + 8, y }, "Application " + std::to_string(ctx.Input().FPS) + " FPS", m_Theme.Text, 14.0f);
		y += 20;
		Label(ctx, { rect.X + 8, y }, "Renderer2D Stats:", m_Theme.TextMuted, 14.0f);
		y += 20;
		Label(ctx, { rect.X + 8, y }, "Draw Calls: " + std::to_string(stats.DrawCalls), m_Theme.Text, 14.0f);
		y += 18;
		Label(ctx, { rect.X + 8, y }, "Quads: " + std::to_string(stats.QuadCount), m_Theme.Text, 14.0f);
		y += 18;
		Label(ctx, { rect.X + 8, y }, "Circles: " + std::to_string(stats.CircleCount), m_Theme.Text, 14.0f);
		y += 18;
		Label(ctx, { rect.X + 8, y }, "Vertices: " + std::to_string(stats.GetTotalVertexCount()), m_Theme.Text, 14.0f);
		y += 18;
		Label(ctx, { rect.X + 8, y }, "Indices: " + std::to_string(stats.GetTotalIndexCount()), m_Theme.Text, 14.0f);
	}

	void EditorShell::DrawMemory(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		const std::vector<AllocatorStats> snapshots = MemoryTracker::Get().GetFullSnapshot();
		const std::vector<float> columns { rect.W * 0.28f, 80, rect.W * 0.2f, rect.W * 0.36f, 70 };
		for (size_t row = 0; row < snapshots.size(); ++row)
		{
			const Wui::WuiRect name = TableCell(rect, columns, row, 0, 24);
			const Wui::WuiRect type = TableCell(rect, columns, row, 1, 24);
			const Wui::WuiRect usage = TableCell(rect, columns, row, 2, 24);
			const Wui::WuiRect bar = TableCell(rect, columns, row, 3, 24);
			const Wui::WuiRect allocs = TableCell(rect, columns, row, 4, 24);
			Label(ctx, { name.X + 6, name.Y + 4 }, snapshots[row].Name, m_Theme.Text, 13.0f);
			Label(ctx, { type.X + 6, type.Y + 4 }, AllocatorTypeName(snapshots[row].Type), m_Theme.TextMuted, 13.0f);
			Label(ctx, { usage.X + 6, usage.Y + 4 }, FormatBytes(snapshots[row].UsedBytes) + " / " + FormatBytes(snapshots[row].TotalReserved), m_Theme.Text, 13.0f);
			const float fraction = snapshots[row].TotalReserved > 0 ? static_cast<float>(snapshots[row].UsedBytes) / static_cast<float>(snapshots[row].TotalReserved) : 0;
			const Wui::WuiRect track { bar.X + 4, bar.Y + 8, bar.W - 8, 8 };
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, track, m_Theme.ButtonBg, 3.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { track.X, track.Y, track.W * fraction, track.H }, m_Theme.Accent, 3.0f });
			Label(ctx, { allocs.X + 6, allocs.Y + 4 }, std::to_string(snapshots[row].NumAllocations), m_Theme.Text, 13.0f);
		}
		MemoryTracker::Get().ClearEphemeralStats();
	}

	void EditorShell::DrawOperations(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		const std::vector<Wui::WuiOpRecord>& records = ctx.Ops().Records();
		const float rowHeight = 16.0f;
		float scrollY = 0;
		BeginScrollArea(ctx, rect, records.size() * rowHeight + 8.0f, scrollY, m_Theme);
		float y = rect.Y + 6 - scrollY;
		for (auto it = records.rbegin(); it != records.rend(); ++it)
		{
			Wui::WuiColor color = m_Theme.TextMuted;
			if (it->Category == "dock") color = m_Theme.Accent;
			else if (it->Category == "undo") color = { 0.35f, 0.8f, 0.45f, 1 };
			else if (it->Category == "drag") color = m_Theme.Text;
			const std::string line = "F" + std::to_string(it->Frame) + " [" + it->Category + "] " + it->Action +
				(it->Target.empty() ? "" : " " + it->Target) + (it->Detail.empty() ? "" : " " + it->Detail);
			Label(ctx, { rect.X + 8, y }, line, color, 12.0f);
			y += rowHeight;
		}
		EndScrollArea(ctx);
		if (Button(ctx, Wui::HashId("ops.clear"), { rect.X + rect.W - 70, rect.Y + 4, 60, 20 }, "Clear", m_Theme))
		{
			ctx.Ops().Clear();
			ctx.RecordOp("ops", "clear", "", "");
		}
	}

	void EditorShell::DrawHierarchy(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		Ref<Scene> scene = m_Editor.GetActiveScene();
		if (!scene)
		{
			Label(ctx, { rect.X + 8, rect.Y + 8 }, "(no scene)", m_Theme.TextMuted, 14.0f);
			return;
		}
		std::vector<Entity> entities;
		for (auto handle : scene->GetRegistry().view<UUIDComponent>())
			entities.push_back(Entity(scene.get(), handle));
		std::sort(entities.begin(), entities.end(), [](Entity a, Entity b)
			{
				const std::string& at = a.GetComponent<TagComponent>().Tag;
				const std::string& bt = b.GetComponent<TagComponent>().Tag;
				return at < bt;
			});

		float scrollY = 0;
		BeginScrollArea(ctx, rect, entities.size() * 22.0f + 8.0f, scrollY, m_Theme);
		Entity contextTarget;
		for (size_t i = 0; i < entities.size(); ++i)
		{
			const Wui::WuiRect row { rect.X + 4, rect.Y + 4 + i * 22.0f - scrollY, rect.W - 8, 22 };
			const bool selected = m_Editor.GetSelectedEntity() == entities[i];
			if (selected || ctx.IsHovered(row))
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, selected ? m_Theme.Accent : m_Theme.ButtonHover, 2.0f });
			const std::string tag = entities[i].GetComponent<TagComponent>().Tag;
			Label(ctx, { row.X + 6, row.Y + 3 }, tag.empty() ? "Empty Entity" : tag, m_Theme.Text, 14.0f);
			if (ctx.IsClicked(row))
				m_Editor.SetSelectedEntity(entities[i]);
			if (ctx.Input().MouseClicked[1] && ctx.IsHovered(row))
			{
				contextTarget = entities[i];
				ctx.OpenPopup(Wui::HashId("hierarchy.context"));
			}
		}
		EndScrollArea(ctx);

		const Wui::WuiId popup = Wui::HashId("hierarchy.context");
		if (ctx.IsPopupOpen(popup) && contextTarget.IsValid())
		{
			ctx.PushOverlay();
			const Wui::WuiRect panel { ctx.Input().MousePos.x, ctx.Input().MousePos.y, 140, 30 };
			const Wui::WuiRect item { panel.X + 4, panel.Y + 4, panel.W - 8, 22 };
			if (MenuItem(ctx, Wui::HashId("hierarchy.delete"), item, "Delete", true, m_Theme))
			{
				Entity::DestroyEntity(scene.get(), contextTarget);
				m_Editor.MarkDocumentDirty();
				ctx.CloseAllPopups();
			}
			ctx.ClosePopupsOnOutsideClick({ popup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(popup);
			ctx.PopOverlay();
		}
	}

	void EditorShell::DrawProperties(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		Entity entity = m_Editor.GetSelectedEntity();
		if (!entity.IsValid() || entity.GetScene() != m_Editor.GetActiveScene().get())
		{
			Label(ctx, { rect.X + 8, rect.Y + 8 }, "No entity selected", m_Theme.TextMuted, 14.0f);
			return;
		}
		Scene* scene = entity.GetScene();
		Schema::SchemaRegistry& schemas = scene->GetContext().Schemas();

		const Wui::WuiRect addButton { rect.X + 8, rect.Y + 8, 140, 24 };
		if (Button(ctx, Wui::HashId("prop.add"), addButton, "Add Component", m_Theme))
			ctx.OpenPopup(Wui::HashId("prop.add.popup"));

		const Wui::WuiId addPopup = Wui::HashId("prop.add.popup");
		if (ctx.IsPopupOpen(addPopup))
		{
			ctx.PushOverlay();
			std::vector<const Schema::TypeSchema*> candidates;
			for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
				if (schema && schema->Storage && !entity.HasComponent(schema->Storage->ComponentId))
					candidates.push_back(schema);
			const Wui::WuiRect panel { addButton.X, addButton.Y + addButton.H, 220, candidates.size() * 22.0f + 8 };
			for (size_t i = 0; i < candidates.size(); ++i)
			{
				const Wui::WuiRect item { panel.X + 4, panel.Y + 4 + i * 22, panel.W - 8, 22 };
				if (MenuItem(ctx, Wui::HashId(("prop.add." + candidates[i]->DisplayName).c_str()), item, candidates[i]->DisplayName, true, m_Theme))
				{
					const uint32_t componentId = candidates[i]->Storage->ComponentId;
					const entt::entity handle = entity;
					if (scene->DeferStructuralChange([handle, componentId](Scene& s)
					{
						Entity target(&s, handle);
						if (target.IsValid() && target.CanAddComponent(componentId))
							target.AddComponent(componentId);
					}))
						m_Editor.MarkDocumentDirty();
					ctx.CloseAllPopups();
				}
			}
			ctx.ClosePopupsOnOutsideClick({ addPopup }, panel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(addPopup);
			ctx.PopOverlay();
		}

		float y = rect.Y + 40;
		for (const Schema::TypeSchema* schema : schemas.List(Schema::TypeCategory::Component))
		{
			if (!schema || !schema->Storage || !entity.HasComponent(schema->Storage->ComponentId))
				continue;
			const Wui::WuiRect fold { rect.X + 6, y, rect.W - 12, 24 };
			bool& open = ctx.Persist<bool>(Wui::HashId(("prop.open." + schema->DisplayName).c_str()), false);
			if (ctx.IsClicked(fold))
				open = !open;
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, fold, open ? m_Theme.ButtonHover : m_Theme.ButtonBg, 2.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { fold.X + 6, fold.Y + 3, 0, 0 }, m_Theme.Text, 0, 1.0f, (open ? "- " : "+ ") + schema->DisplayName, 14.0f, false });
			y += 26;
			if (open)
			{
				const Wui::WuiRect inner { rect.X + 14, y, rect.W - 28, 0 };
				y += DrawComponentInspector(ctx, inner, entity, *schema);
			}
		}
	}

	float EditorShell::DrawSchemaFields(Wui::WuiContext& ctx, Wui::WuiId base, const Wui::WuiRect& rect,
		void* instance, const std::string& typeName, const Schema::TypeSchema& schema)
	{
		float y = 0;
		bool changed = false;
		const float labelWidth = std::min(140.0f, rect.W * 0.45f);
		for (const Schema::FieldSchema& field : schema.Fields)
		{
			if (field.Meta.Transient)
				continue;
			const Wui::WuiId fid = Wui::HashId(("f." + typeName + "." + field.Name).c_str()) ^ base;
			const Wui::WuiRect row { rect.X, rect.Y + y, rect.W, 22 };
			const Wui::WuiRect ctrl { row.X + labelWidth, row.Y + 1, row.W - labelWidth - 4, 20 };
			const std::string label = field.Meta.DisplayName.empty() ? field.Name : field.Meta.DisplayName;

			if (field.K == Schema::Kind::Object)
			{
				const Schema::TypeSchema* nested = field.GetNested ? field.GetNested() : nullptr;
				void* nestedInstance = field.GetPtr ? field.GetPtr(instance) : nullptr;
				bool& open = ctx.Persist<bool>(fid, false);
				if (ctx.IsClicked(row))
					open = !open;
				ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { row.X + 4, row.Y + 3, 0, 0 }, m_Theme.Text, 0, 1.0f, (open ? "- " : "+ ") + label, 13.0f, false });
				y += 20;
				if (open && nested && nestedInstance)
					y += DrawSchemaFields(ctx, fid ^ 0x9e3779b9u, { row.X + 10, row.Y + 20, row.W - 10, 0 }, nestedInstance, nested->DisplayName, *nested);
				continue;
			}

			if (field.Meta.ReadOnly || !field.Get || !field.Set)
			{
				Label(ctx, { row.X + 4, row.Y + 3 }, label, m_Theme.TextMuted, 13.0f);
				y += 20;
				continue;
			}

			Label(ctx, { row.X + 4, row.Y + 3 }, label, m_Theme.TextMuted, 13.0f);
			Schema::Value value = field.Get(instance);
			bool fieldChanged = false;
			switch (field.K)
			{
				case Schema::Kind::Bool:
				{
					bool b = std::get<bool>(value);
					const bool before = b;
					Checkbox(ctx, fid, ctrl, "", b, m_Theme);
					fieldChanged = b != before;
					if (fieldChanged) value = b;
					break;
				}
				case Schema::Kind::Int8:
				case Schema::Kind::Int16:
				case Schema::Kind::Int32:
				case Schema::Kind::Int64:
				{
					int64_t raw = field.K == Schema::Kind::Int8 ? std::get<int8_t>(value)
						: field.K == Schema::Kind::Int16 ? std::get<int16_t>(value)
						: field.K == Schema::Kind::Int32 ? std::get<int32_t>(value) : std::get<int64_t>(value);
					const int64_t before = raw;
					const int64_t lo = field.Meta.Min.has_value() ? static_cast<int64_t>(*field.Meta.Min) : INT64_MIN;
					const int64_t hi = field.Meta.Max.has_value() ? static_cast<int64_t>(*field.Meta.Max) : INT64_MAX;
					DragInt(ctx, fid, ctrl, raw, lo, hi, m_Theme);
					fieldChanged = raw != before;
					if (fieldChanged)
					{
						if (field.K == Schema::Kind::Int8) value = static_cast<int8_t>(raw);
						else if (field.K == Schema::Kind::Int16) value = static_cast<int16_t>(raw);
						else if (field.K == Schema::Kind::Int32) value = static_cast<int32_t>(raw);
						else value = raw;
					}
					break;
				}
				case Schema::Kind::UInt8:
				case Schema::Kind::UInt16:
				case Schema::Kind::UInt32:
				case Schema::Kind::UInt64:
				{
					uint64_t raw = field.K == Schema::Kind::UInt8 ? std::get<uint8_t>(value)
						: field.K == Schema::Kind::UInt16 ? std::get<uint16_t>(value)
						: field.K == Schema::Kind::UInt32 ? std::get<uint32_t>(value) : std::get<uint64_t>(value);
					int64_t signedRaw = static_cast<int64_t>(raw);
					const int64_t before = signedRaw;
					const int64_t lo = field.Meta.Min.has_value() ? static_cast<int64_t>(*field.Meta.Min) : 0;
					const int64_t hi = field.Meta.Max.has_value() ? static_cast<int64_t>(*field.Meta.Max) : INT64_MAX;
					DragInt(ctx, fid, ctrl, signedRaw, lo, hi, m_Theme);
					fieldChanged = signedRaw != before;
					if (fieldChanged)
					{
						if (field.K == Schema::Kind::UInt8) value = static_cast<uint8_t>(signedRaw);
						else if (field.K == Schema::Kind::UInt16) value = static_cast<uint16_t>(signedRaw);
						else if (field.K == Schema::Kind::UInt32) value = static_cast<uint32_t>(signedRaw);
						else value = static_cast<uint64_t>(signedRaw);
					}
					break;
				}
				case Schema::Kind::Float:
				case Schema::Kind::Double:
				{
					float f = field.K == Schema::Kind::Float ? std::get<float>(value) : static_cast<float>(std::get<double>(value));
					const float before = f;
					const float lo = field.Meta.Min.has_value() ? *field.Meta.Min : 1.0f;   // 1,-1 哨兵 = 无范围
					const float hi = field.Meta.Max.has_value() ? *field.Meta.Max : -1.0f;
					DragFloat(ctx, fid, ctrl, f, 0.01f, lo, hi, m_Theme);
					fieldChanged = f != before;
					if (fieldChanged) value = field.K == Schema::Kind::Float ? Schema::Value(f) : Schema::Value(static_cast<double>(f));
					break;
				}
				case Schema::Kind::Vec2:
				case Schema::Kind::Vec3:
				case Schema::Kind::Vec4:
				{
					const int components = field.K == Schema::Kind::Vec2 ? 2 : (field.K == Schema::Kind::Vec3 ? 3 : 4);
					const float slot = ctrl.W / components;
					for (int c = 0; c < components; ++c)
					{
						float f = field.K == Schema::Kind::Vec2 ? std::get<glm::vec2>(value)[c]
							: field.K == Schema::Kind::Vec3 ? std::get<glm::vec3>(value)[c] : std::get<glm::vec4>(value)[c];
						const float before = f;
						DragFloat(ctx, fid ^ static_cast<Wui::WuiId>(c + 1), { ctrl.X + slot * c, ctrl.Y, slot - 2, ctrl.H }, f, 0.01f, 1.0f, -1.0f, m_Theme);
						if (f != before)
						{
							fieldChanged = true;
							if (field.K == Schema::Kind::Vec2) std::get<glm::vec2>(value)[c] = f;
							else if (field.K == Schema::Kind::Vec3) std::get<glm::vec3>(value)[c] = f;
							else std::get<glm::vec4>(value)[c] = f;
						}
					}
					break;
				}
				case Schema::Kind::String:
				case Schema::Kind::Asset:
				{
					std::string s = std::get<std::string>(value);
					const std::string before = s;
					TextField(ctx, fid, ctrl, s, m_Theme);
					fieldChanged = s != before;
					if (fieldChanged) value = s;
					break;
				}
				case Schema::Kind::Enum:
				{
					const Schema::EnumSchema* es = field.GetEnum ? field.GetEnum() : nullptr;
					if (es)
					{
						std::vector<std::string> names;
						int selected = 0;
						const int64_t raw = es->IsSigned ? std::get<int64_t>(value) : static_cast<int64_t>(std::get<uint64_t>(value));
						for (size_t i = 0; i < es->Values.size(); ++i)
						{
							names.push_back(es->Values[i].first);
							if (es->Values[i].second == raw)
								selected = static_cast<int>(i);
						}
						const int before = selected;
						Combo(ctx, fid, ctrl, "", names, selected, m_Theme);
						fieldChanged = selected != before;
						if (fieldChanged)
							value = es->IsSigned ? Schema::Value(es->Values[selected].second) : Schema::Value(static_cast<uint64_t>(es->Values[selected].second));
					}
					break;
				}
				default:
					Label(ctx, { ctrl.X, ctrl.Y + 3 }, "(unsupported)", m_Theme.TextMuted, 12.0f);
					break;
			}
			if (fieldChanged)
			{
				field.Set(instance, value);
				changed = true;
			}
			y += 22;
		}

		if (changed)
		{
			if (typeName == "TransformComponent")
				static_cast<TransformComponent*>(instance)->RecalculateTransform();
			else if (typeName == "SceneCamera")
				static_cast<SceneCamera*>(instance)->ApplyEdit();
			m_Editor.MarkDocumentDirty();
		}
		return y;
	}

	float EditorShell::DrawComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, const Schema::TypeSchema& schema)
	{
		void* instance = entity.GetComponent(schema.Storage->ComponentId);
		if (!instance)
			return 0;
		const Wui::WuiId base = Wui::HashId(schema.DisplayName.c_str());

		if (schema.Id.Name == "World::NativeScriptComponent")
		{
			auto* script = static_cast<NativeScriptComponent*>(instance);
			Scene* scene = entity.GetScene();
			Schema::SchemaRegistry& schemas = scene->GetContext().Schemas();
			std::vector<std::string> names;
			std::vector<const Schema::TypeSchema*> scripts = schemas.List(Schema::TypeCategory::Script);
			int selected = -1;
			for (size_t i = 0; i < scripts.size(); ++i)
			{
				names.push_back(scripts[i]->DisplayName);
				if (scripts[i]->DisplayName == script->ScriptName)
					selected = static_cast<int>(i);
			}
			if (Combo(ctx, base ^ 1u, { rect.X, rect.Y, rect.W, 22 }, "Script", names, selected, m_Theme) && selected >= 0)
			{
				if (scripts[selected]->Script)
					scripts[selected]->Script->Bind(static_cast<void*>(script));
				script->ScriptName = names[selected];
				script->ResetEditorFieldState();
				m_Editor.MarkDocumentDirty();
			}
			float y = 26;
			Label(ctx, { rect.X, rect.Y + y }, "state: " + ScriptStateName(script->State), m_Theme.TextMuted, 13.0f);
			y += 18;
			if (!script->LastError.empty())
			{
				Label(ctx, { rect.X, rect.Y + y }, script->LastError, { 1, 0.4f, 0.4f, 1 }, 12.0f);
				y += 18;
			}
			const Schema::TypeSchema* scriptSchema = schemas.Find(script->ScriptName);
			if (scriptSchema)
			{
				bool owned = false;
				ScriptableEntity* preview = script->GetOrCreateEditorInstance(!scene->IsActive(), owned);
				if (preview)
				{
					y += DrawSchemaFields(ctx, base ^ 2u, { rect.X, rect.Y + y, rect.W, 0 }, preview, scriptSchema->DisplayName, *scriptSchema);
					for (const Schema::FieldSchema& field : scriptSchema->Fields)
						if (field.Get)
							script->FieldValues[field.Name] = field.Get(preview);
					script->ReleaseEditorInstance(preview);
				}
			}
			return y;
		}

		if (schema.Id.Name == "World::LuaScriptComponent")
		{
			auto* script = static_cast<LuaScriptComponent*>(instance);
			const std::string before = script->ScriptFilePath;
			TextField(ctx, base ^ 1u, { rect.X, rect.Y, rect.W, 22 }, script->ScriptFilePath, m_Theme);
			if (script->ScriptFilePath != before)
				m_Editor.MarkDocumentDirty();
			Label(ctx, { rect.X, rect.Y + 26 }, "state: " + ScriptStateName(script->State), m_Theme.TextMuted, 13.0f);
			return 46;
		}

		return DrawSchemaFields(ctx, base, rect, instance, schema.DisplayName, schema);
	}

	void EditorShell::DrawViewport(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		m_ViewportRect = rect;
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, { 0.06f, 0.06f, 0.07f, 1 }, 0.0f });
		if (m_Editor.HasRenderedScene() && m_Editor.GetSceneRenderer())
		{
			const uint64_t texture = m_Editor.GetSceneRenderer()->GetTargetFramebuffer()->GetColorAttachmentRendererID();
			Image(ctx, rect, texture, { 0, 1, 1, -1 }, m_Theme);
		}

		const bool hovered = ctx.IsHovered(rect);
		if (ctx.IsClicked(rect))
			ctx.SetFocus(Wui::HashId("viewport"));
		const bool focused = ctx.Focus() == Wui::HashId("viewport");
		glm::vec2 bounds[2] = { { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } };
		m_Editor.SetViewportState(focused, hovered, { rect.W, rect.H }, bounds);

		// 悬浮工具栏:Play / Simulate / Pause
		const float panelWidth = 3 * 28.0f + 2 * 8.0f + 16.0f;
		const Wui::WuiRect bar { rect.X + (rect.W - panelWidth) * 0.5f, rect.Y + 14, panelWidth, 44 };
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, bar, { 0.12f, 0.12f, 0.12f, 0.85f }, 6.0f });
		const bool play = m_Editor.IsPlaying();
		const bool simulate = m_Editor.IsSimulating();
		const bool paused = m_Editor.IsPaused();
		struct Tool { int Icon; std::function<void()> Action; bool Dim; };
		const Tool tools[] = {
			{ play ? 1 : 0, [this] { m_Editor.TogglePlay(); }, simulate },
			{ simulate ? 5 : 4, [this] { m_Editor.ToggleSimulate(); }, play },
			{ paused ? (simulate ? 7 : 3) : (simulate ? 6 : 2), [this] { m_Editor.TogglePause(); }, !play && !simulate },
		};
		for (int i = 0; i < 3; ++i)
		{
			const Wui::WuiRect button { bar.X + 8 + i * 36, bar.Y + 8, 28, 28 };
			Ref<Texture2D> icon = m_Editor.GetIcon(tools[i].Icon);
			if (icon)
			{
				Image(ctx, button, icon->GetRendererID(), { 0, 1, 1, -1 }, m_Theme);
				if (!tools[i].Dim && ctx.IsClicked(button))
					tools[i].Action();
			}
		}

		if (ctx.IsClicked(rect) && !ImGuiDrawLibrary::GizmoIsOver())
		{
			const glm::vec2 local = ctx.Input().MousePos - glm::vec2 { rect.X, rect.Y };
			m_Editor.SetSelectedEntity(m_Editor.PickEntityAt(local));
		}

		Entity selected = m_Editor.GetSelectedEntity();
		if (selected.IsValid() && selected.GetScene() == m_Editor.GetActiveScene().get() &&
			selected.HasComponent<TransformComponent>() && m_Editor.HasRenderedScene())
		{
			auto& transform = selected.GetComponent<TransformComponent>();
			const bool wasUsing = ImGuiDrawLibrary::GizmoIsUsing();
			const TransformComponent before = transform;
			ImGuiDrawLibrary::DrawGizmo(m_Editor.GetEditorCamera(), selected, static_cast<ImGuizmo::OPERATION>(m_Editor.GetGizmoOperation()), rect);
			const bool nowUsing = ImGuiDrawLibrary::GizmoIsUsing();
			if (!wasUsing && nowUsing)
			{
				m_GizmoActive = true;
				m_GizmoBefore = before;
			}
			else if (wasUsing && !nowUsing && m_GizmoActive)
			{
				m_GizmoActive = false;
				const auto& after = selected.GetComponent<TransformComponent>();
				if (after.Location != m_GizmoBefore.Location || after.Rotation != m_GizmoBefore.Rotation || after.Scale != m_GizmoBefore.Scale)
					m_Editor.MarkDocumentDirty();
			}
		}
	}

	void EditorShell::UpdateBrowserSearch()
	{
		m_Browser.SearchResults.clear();
		std::string query = m_Browser.Search;
		std::transform(query.begin(), query.end(), query.begin(), ::tolower);
		std::error_code searchError;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(m_Browser.Current, std::filesystem::directory_options::skip_permission_denied, searchError))
		{
			if (searchError)
				break;
			std::string name = entry.path().filename().string();
			std::transform(name.begin(), name.end(), name.begin(), ::tolower);
			if (name.find(query) != std::string::npos)
				m_Browser.SearchResults.push_back(entry.path());
		}
	}

	void EditorShell::RefreshBrowserTree(bool force)
	{
		const auto now = std::chrono::steady_clock::now();
		if (!force && !m_Browser.DirTreeDirty && std::chrono::duration<double>(now - m_Browser.LastTreeCheck).count() < 0.5)
			return;
		m_Browser.LastTreeCheck = now;

		// 目录树只在 Root 直接内容变化或显式标脏时重扫,避免每帧全量递归。
		std::error_code stampError;
		const auto stamp = std::filesystem::last_write_time(m_Browser.Root, stampError);
		if (!force && !m_Browser.DirTreeDirty && !stampError && stamp == m_Browser.TreeStamp)
			return;

		m_Browser.DirTree.clear();
		std::error_code scanError;
		std::filesystem::recursive_directory_iterator scanIt(m_Browser.Root, std::filesystem::directory_options::skip_permission_denied, scanError);
		const std::filesystem::recursive_directory_iterator scanEnd;
		for (; scanIt != scanEnd; scanIt.increment(scanError))
		{
			if (scanError)
				break;
			const auto& entry = *scanIt;
			std::error_code dirError;
			if (!entry.is_directory(dirError))
				continue;
			BrowserDirNode node;
			node.Path = entry.path();
			// depth():根的直接子项为 0;顶层目录显示深度记为 1。
			node.Depth = scanIt.depth() + 1;
			node.HasChildren = false;
			std::error_code childError;
			for (const auto& child : std::filesystem::directory_iterator(node.Path, std::filesystem::directory_options::skip_permission_denied, childError))
			{
				if (childError)
					break;
				std::error_code childDirError;
				if (child.is_directory(childDirError)) { node.HasChildren = true; break; }
			}
			m_Browser.DirTree.push_back(std::move(node));
		}
		std::sort(m_Browser.DirTree.begin(), m_Browser.DirTree.end(), [](const BrowserDirNode& a, const BrowserDirNode& b) { return a.Path < b.Path; });
		m_Browser.TreeStamp = stamp;
		m_Browser.DirTreeDirty = false;
	}

	void EditorShell::RefreshBrowserListing()
	{
		const auto now = std::chrono::steady_clock::now();
		if (!m_Browser.ListingDirty && m_Browser.ListingPath == m_Browser.Current && std::chrono::duration<double>(now - m_Browser.LastListingCheck).count() < 0.5)
			return;
		m_Browser.LastListingCheck = now;

		std::error_code stampError;
		const auto stamp = std::filesystem::last_write_time(m_Browser.Current, stampError);
		if (!m_Browser.ListingDirty && m_Browser.ListingPath == m_Browser.Current && !stampError && stamp == m_Browser.ListingStamp)
			return;

		m_Browser.Listing.clear();
		std::error_code listError;
		for (const auto& entry : std::filesystem::directory_iterator(m_Browser.Current, std::filesystem::directory_options::skip_permission_denied, listError))
		{
			if (listError)
				break;
			m_Browser.Listing.push_back(entry.path());
		}
		std::sort(m_Browser.Listing.begin(), m_Browser.Listing.end());
		m_Browser.ListingPath = m_Browser.Current;
		m_Browser.ListingStamp = stamp;
		m_Browser.ListingDirty = false;
	}

	uintmax_t EditorShell::BrowserFileSize(const std::filesystem::path& path)
	{
		std::error_code stampError;
		const auto stamp = std::filesystem::last_write_time(path, stampError);
		const auto cached = m_Browser.SizeCache.find(path);
		if (!stampError && cached != m_Browser.SizeCache.end() && cached->second.first == stamp)
			return cached->second.second;
		std::error_code sizeError;
		const uintmax_t size = std::filesystem::file_size(path, sizeError);
		const uintmax_t result = sizeError ? 0 : size;
		m_Browser.SizeCache[path] = { stamp, result };
		return result;
	}

	void EditorShell::InvalidateBrowserContents()
	{
		m_Browser.DirTreeDirty = true;
		m_Browser.ListingDirty = true;
		m_Browser.TreeStamp = {};
		m_Browser.ListingStamp = {};
	}

	void EditorShell::SaveBrowserState()
	{
		try
		{
			Wui::JsonValue root;
			root.type = Wui::JsonValue::Type::Object;
			root.Object.push_back({ "listMode", Wui::JsonValue::MakeBool(m_Browser.ListMode) });
			root.Object.push_back({ "current", Wui::JsonValue::MakeString(m_Browser.Current.lexically_relative(m_Browser.Root).generic_string()) });
			root.Object.push_back({ "treeScroll", Wui::JsonValue::MakeNumber(m_Browser.TreeScroll) });
			root.Object.push_back({ "contentScroll", Wui::JsonValue::MakeNumber(m_Browser.ContentScroll) });
			Wui::JsonValue open;
			open.type = Wui::JsonValue::Type::Array;
			for (const auto& path : m_Browser.TreeOpen)
				open.Array.push_back(Wui::JsonValue::MakeString(path.lexically_relative(m_Browser.Root).generic_string()));
			root.Object.push_back({ "treeOpen", std::move(open) });
			std::ofstream stream(m_BrowserPath, std::ios::binary | std::ios::trunc);
			if (stream)
				stream << root.Dump();
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Failed to save content browser state: {0}", error.what());
		}
	}

	void EditorShell::LoadBrowserState()
	{
		if (!std::filesystem::exists(m_BrowserPath))
			return;
		try
		{
			std::ifstream stream(m_BrowserPath, std::ios::binary);
			if (!stream)
				return;
			std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			std::string error;
			const auto parsed = Wui::JsonValue::Parse(text, &error);
			if (!parsed)
				return;
			if (const Wui::JsonValue* value = parsed->Find("listMode"))
				m_Browser.ListMode = value->AsBool(false);
			if (const Wui::JsonValue* value = parsed->Find("current"))
			{
				const std::string relative = value->AsString("");
				if (!relative.empty())
				{
					const std::filesystem::path target = m_Browser.Root / std::filesystem::path(relative);
					std::error_code dirError;
					if (std::filesystem::is_directory(target, dirError))
						m_Browser.Current = target;
				}
			}
			if (const Wui::JsonValue* value = parsed->Find("treeScroll"))
				m_Browser.TreeScroll = static_cast<float>(value->AsNumber(0));
			if (const Wui::JsonValue* value = parsed->Find("contentScroll"))
				m_Browser.ContentScroll = static_cast<float>(value->AsNumber(0));
			if (const Wui::JsonValue* value = parsed->Find("treeOpen"))
			{
				m_Browser.TreeOpen.clear();
				for (const auto& item : value->Array)
				{
					const std::string relative = item.AsString("");
					if (!relative.empty())
						m_Browser.TreeOpen.insert(m_Browser.Root / std::filesystem::path(relative));
				}
			}
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Failed to load content browser state: {0}", error.what());
		}
	}

	void EditorShell::BrowserNavigate(const std::filesystem::path& path)
	{
		if (m_Browser.HistoryIndex >= 0 && m_Browser.HistoryIndex < static_cast<int>(m_Browser.History.size()) && m_Browser.History[m_Browser.HistoryIndex] == path)
			return;
		if (m_Browser.HistoryIndex < static_cast<int>(m_Browser.History.size()) - 1)
			m_Browser.History.resize(m_Browser.HistoryIndex + 1);
		m_Browser.History.push_back(path);
		m_Browser.HistoryIndex = static_cast<int>(m_Browser.History.size()) - 1;
		m_Browser.Current = path;
		m_Browser.Selected.clear();
		m_Browser.LastSelected.clear();
		BrowserReveal(path);
		m_Browser.ListingDirty = true;
		m_Browser.ListingStamp = {};
		SaveBrowserState();
		if (m_Browser.Search[0])
			UpdateBrowserSearch();
	}

	void EditorShell::BrowserReveal(const std::filesystem::path& path)
	{
		// 展开路径上所有父级,让目录树始终能看到当前所在位置。
		std::filesystem::path current = path;
		while (current != m_Browser.Root && !current.empty() && current.has_parent_path())
		{
			m_Browser.TreeOpen.insert(current);
			const std::filesystem::path parent = current.parent_path();
			if (parent == current)
				break;
			current = parent;
		}
	}

	void EditorShell::BrowserGoBack()
	{
		if (m_Browser.HistoryIndex > 0)
		{
			--m_Browser.HistoryIndex;
			m_Browser.Current = m_Browser.History[m_Browser.HistoryIndex];
			m_Browser.Selected.clear();
			m_Browser.LastSelected.clear();
			BrowserReveal(m_Browser.Current);
			m_Browser.ListingDirty = true;
			m_Browser.ListingStamp = {};
			SaveBrowserState();
			if (m_Browser.Search[0])
				UpdateBrowserSearch();
		}
	}

	void EditorShell::BrowserGoUp()
	{
		if (m_Browser.Current != m_Browser.Root)
			BrowserNavigate(m_Browser.Current.parent_path());
	}

	void EditorShell::BrowserOpenItem(const std::filesystem::path& path)
	{
		if (std::filesystem::is_directory(path))
		{
			BrowserNavigate(path);
			return;
		}
		if (path.extension() == ".wd")
			m_Editor.OpenScene(path);
		else
		{
			const std::string cmd = "start \"\" \"" + std::filesystem::absolute(path).string() + "\"";
			system(cmd.c_str());
		}
	}


	void EditorShell::BrowserPasteInto(const std::filesystem::path& destination)
	{
		for (const auto& path : m_Browser.Clipboard)
		{
			if (!std::filesystem::exists(path))
				continue;
			std::filesystem::path destPath = destination / path.filename();
			if (m_Browser.ClipboardCut)
			{
				if (path != destPath)
				{
					std::filesystem::rename(path, destPath);
					m_Ctx->RecordOp("browser", "move", path.filename().string(), "-> " + destination.string());
				}
			}
			else
			{
				std::string stem = path.stem().string();
				std::string ext = path.extension().string();
				int counter = 1;
				while (std::filesystem::exists(destPath))
					destPath = destination / (stem + "-Copy(" + std::to_string(counter++) + ")" + ext);
				std::filesystem::copy(path, destPath, std::filesystem::copy_options::recursive);
				m_Ctx->RecordOp("browser", "copy", path.filename().string(), "-> " + destPath.string());
			}
		}
		if (m_Browser.ClipboardCut)
		{
			m_Browser.Clipboard.clear();
			m_Browser.ClipboardCut = false;
		}
		InvalidateBrowserContents();
		SaveBrowserState();
		if (m_Browser.Search[0])
			UpdateBrowserSearch();
	}

	void EditorShell::BrowserDeleteSelection()
	{
		const size_t count = m_Browser.Selected.size();
		for (const auto& path : m_Browser.Selected)
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
		m_Browser.Selected.clear();
		m_Browser.LastSelected.clear();
		InvalidateBrowserContents();
		SaveBrowserState();
		if (m_Browser.Search[0])
			UpdateBrowserSearch();
		m_Ctx->RecordOp("browser", "delete", std::to_string(count), "");
	}

	void EditorShell::BrowserCreateFolder()
	{
		std::filesystem::path newPath = m_Browser.Current / "New Folder";
		int counter = 1;
		while (std::filesystem::exists(newPath))
			newPath = m_Browser.Current / ("New Folder (" + std::to_string(counter++) + ")");
		try
		{
			std::filesystem::create_directory(newPath);
			m_Browser.RenameTarget = newPath;
			std::strncpy(m_Browser.RenameBuffer, newPath.filename().string().c_str(), sizeof(m_Browser.RenameBuffer) - 1);
			m_Browser.Selected.clear();
			m_Browser.Selected.insert(newPath);
			m_Browser.LastSelected = newPath;
			InvalidateBrowserContents();
			SaveBrowserState();
			m_Ctx->RecordOp("browser", "mkdir", newPath.filename().string(), "");
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("Could not create directory: {0}", error.what());
		}
	}

	void EditorShell::BrowserApplyRename(const std::filesystem::path& target, const std::string& newName)
	{
		if (newName.empty())
		{
			m_Browser.RenameTarget.clear();
			return;
		}
		const std::filesystem::path newPath = target.parent_path() / newName;
		if (newPath != target)
		{
			std::error_code ignored;
			std::filesystem::rename(target, newPath, ignored);
			m_Ctx->RecordOp("browser", "rename", target.filename().string(), "-> " + newPath.filename().string());
		}
		m_Browser.RenameTarget.clear();
		InvalidateBrowserContents();
		SaveBrowserState();
		if (m_Browser.Search[0])
			UpdateBrowserSearch();
	}

	void EditorShell::BrowserCut()
	{
		m_Browser.Clipboard.assign(m_Browser.Selected.begin(), m_Browser.Selected.end());
		m_Browser.ClipboardCut = true;
	}

	void EditorShell::BrowserCopy()
	{
		m_Browser.Clipboard.assign(m_Browser.Selected.begin(), m_Browser.Selected.end());
		m_Browser.ClipboardCut = false;
	}

	void EditorShell::BrowserSelectAll(const std::vector<std::filesystem::path>& paths)
	{
		m_Browser.Selected.clear();
		m_Browser.Selected.insert(paths.begin(), paths.end());
		if (!paths.empty())
			m_Browser.LastSelected = paths.back();
	}

	void EditorShell::DrawContentBrowser(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		if (!m_Browser.DirIcon)
			m_Browser.DirIcon = Texture2D::Create("Resource/Icons/ContentBrowser/DirectoryIcon.png");
		if (!m_Browser.FileIcon)
			m_Browser.FileIcon = Texture2D::Create("Resource/Icons/ContentBrowser/FileIcon.png");

		std::string filePayload;
		const bool fileDrag = ctx.IsDragActive(&filePayload) && filePayload.rfind("file:", 0) == 0;

		// ---- 顶部工具栏 ----
		float y = rect.Y + 6;
		const bool canBack = m_Browser.HistoryIndex > 0;
		const bool canForward = m_Browser.HistoryIndex < static_cast<int>(m_Browser.History.size()) - 1;
		if (Button(ctx, Wui::HashId("browser.back"), { rect.X + 6, y, 24, 24 }, "<", m_Theme) && canBack)
			BrowserGoBack();
		if (Button(ctx, Wui::HashId("browser.forward"), { rect.X + 32, y, 24, 24 }, ">", m_Theme) && canForward)
		{
			++m_Browser.HistoryIndex;
			m_Browser.Current = m_Browser.History[m_Browser.HistoryIndex];
			m_Browser.Selected.clear();
			m_Browser.LastSelected.clear();
			BrowserReveal(m_Browser.Current);
			m_Browser.ListingDirty = true;
			m_Browser.ListingStamp = {};
			SaveBrowserState();
			if (m_Browser.Search[0])
				UpdateBrowserSearch();
		}
		if (Button(ctx, Wui::HashId("browser.up"), { rect.X + 58, y, 32, 24 }, "Up", m_Theme) && m_Browser.Current != m_Browser.Root)
			BrowserGoUp();

		float x = rect.X + 96;
		auto breadcrumb = [&](const std::string& label, const std::filesystem::path& destination, const Wui::WuiId id)
		{
			const Wui::WuiRect btn { x, y, static_cast<float>(label.size() * 8 + 20), 24 };
			if (fileDrag && ctx.IsHovered(btn))
			{
				ctx.DropTarget(btn, "file:");
				m_Browser.PendingDropDest = destination;
				ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, btn, m_Theme.Accent, 2.0f, 2.0f });
			}
			if (Button(ctx, id, btn, label, m_Theme))
				BrowserNavigate(destination);
			x += btn.W + 6;
		};
		breadcrumb("Root", m_Browser.Root, Wui::HashId("browser.crumb.root"));
		std::filesystem::path accumulated = m_Browser.Root;
		for (const auto& part : m_Browser.Current.lexically_relative(m_Browser.Root))
		{
			accumulated /= part;
			breadcrumb(part.string(), accumulated, Wui::HashId(("browser.crumb." + part.string()).c_str()));
		}

		if (Button(ctx, Wui::HashId("browser.viewmode"), { rect.X + rect.W - 372, y, 58, 24 }, m_Browser.ListMode ? "Grid" : "List", m_Theme))
		{
			m_Browser.ListMode = !m_Browser.ListMode;
			SaveBrowserState();
		}
		if (Button(ctx, Wui::HashId("browser.newfolder"), { rect.X + rect.W - 308, y, 70, 24 }, "+ Folder", m_Theme))
			BrowserCreateFolder();
		if (Button(ctx, Wui::HashId("browser.refresh"), { rect.X + rect.W - 232, y, 58, 24 }, "Refresh", m_Theme))
		{
			InvalidateBrowserContents();
			if (m_Browser.Search[0])
				UpdateBrowserSearch();
		}
		{
			std::string query = m_Browser.Search;
			if (TextField(ctx, Wui::HashId("browser.search"), { rect.X + rect.W - 168, y, 162, 24 }, query, m_Theme))
			{
				std::strncpy(m_Browser.Search, query.c_str(), sizeof(m_Browser.Search) - 1);
				UpdateBrowserSearch();
			}
		}
		y += 32;

		// ---- 左侧目录树 ----
		const float treeW = 190;
		const Wui::WuiRect treeRect { rect.X, y, treeW, rect.H - (y - rect.Y) };
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, treeRect, { 0.09f, 0.095f, 0.10f, 1 }, 0.0f });
		RefreshBrowserTree(false);
		BeginScrollArea(ctx, treeRect, m_Browser.DirTree.size() * 20.0f + 8, m_Browser.TreeScroll, m_Theme);
		float ty = treeRect.Y + 4 - m_Browser.TreeScroll;
		for (const BrowserDirNode& node : m_Browser.DirTree)
		{
			// 顶层目录恒可见;更深层目录仅在父级展开时显示。
			if (node.Depth > 1 && m_Browser.TreeOpen.find(node.Path.parent_path()) == m_Browser.TreeOpen.end())
				continue;
			const Wui::WuiRect row { treeRect.X + 4 + node.Depth * 12, ty, treeW - 8 - node.Depth * 12, 20 };
			const bool isCurrent = m_Browser.Current == node.Path;
			if (isCurrent)
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, m_Theme.ButtonHover, 2.0f });
			const bool hovered = ctx.IsHovered(row);
			if (hovered && !isCurrent)
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, { 1, 1, 1, 0.06f }, 2.0f });
			if (node.HasChildren)
			{
				const Wui::WuiRect arrow { row.X, ty, 16, 20 };
				const bool open = m_Browser.TreeOpen.find(node.Path) != m_Browser.TreeOpen.end();
				if (ctx.IsClicked(arrow))
				{
					if (open) m_Browser.TreeOpen.erase(node.Path);
					else m_Browser.TreeOpen.insert(node.Path);
					SaveBrowserState();
				}
				Label(ctx, { row.X, ty + 2 }, open ? "[-]" : "[+]", m_Theme.TextMuted, 12.0f);
			}
			const float labelX = node.HasChildren ? row.X + 18 : row.X + 2;
			if (ctx.IsClicked({ labelX, ty, row.W - (labelX - row.X), 20 }))
				BrowserNavigate(node.Path);
			// 树中的目录既可作为拖拽源,也可作为文件/文件夹的落点。
			if (ctx.Input().MouseDown[0] && hovered && node.Path != m_Browser.Root)
			{
				const std::filesystem::path rel = node.Path.lexically_relative(m_Browser.Root);
				ctx.BeginDrag(Wui::HashId(("browser.drag." + rel.string()).c_str()), "file:" + rel.string());
				ctx.SetCursor(Wui::WuiCursor::Hand);
			}
			if (fileDrag && hovered)
			{
				ctx.DropTarget(row, "file:");
				m_Browser.PendingDropDest = node.Path;
				ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, row, m_Theme.Accent, 2.0f, 2.0f });
			}
			Label(ctx, { labelX, ty + 2 }, node.Path.filename().string(), m_Theme.Text, 13.0f);
			ty += 20;
		}
		EndScrollArea(ctx);

		// ---- 右侧内容区 ----
		const Wui::WuiRect content { rect.X + treeW + 6, y, rect.W - treeW - 6, rect.H - (y - rect.Y) };
		const bool searching = m_Browser.Search[0] != 0;
		if (!searching)
			RefreshBrowserListing();
		const std::vector<std::filesystem::path>& paths = searching ? m_Browser.SearchResults : m_Browser.Listing;

		if (ctx.Input().Ctrl && ctx.IsKeyPressed(KeyCodes::A) && ctx.IsHovered(content))
			BrowserSelectAll(paths);
		if (ctx.IsKeyPressed(KeyCodes::Delete) && !m_Browser.Selected.empty() && ctx.IsHovered(content))
			m_Browser.ShowDeleteModal = true;
		if (ctx.IsKeyPressed(KeyCodes::F2) && m_Browser.Selected.size() == 1 && ctx.IsHovered(content))
		{
			m_Browser.RenameTarget = *m_Browser.Selected.begin();
			std::strncpy(m_Browser.RenameBuffer, m_Browser.RenameTarget.filename().string().c_str(), sizeof(m_Browser.RenameBuffer) - 1);
		}

		if (fileDrag && ctx.IsHovered(content))
		{
			ctx.DropTarget(content, "file:");
			m_Browser.PendingDropDest = m_Browser.Current;
			ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, content, m_Theme.Accent, 0.0f, 2.0f });
		}

		std::filesystem::path contextPath;
		auto interact = [&](const std::filesystem::path& path, const Wui::WuiRect& itemRect, bool isDir)
		{
			const bool selected = m_Browser.Selected.find(path) != m_Browser.Selected.end();
			const bool hovered = ctx.IsHovered(itemRect);
			if (isDir && fileDrag && hovered)
			{
				ctx.DropTarget(itemRect, "file:");
				m_Browser.PendingDropDest = path;
				ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, itemRect, m_Theme.Accent, 2.0f, 2.0f });
			}
			if (ctx.Input().MouseDown[0] && hovered)
			{
				const std::filesystem::path rel = path.lexically_relative(m_Browser.Root);
				ctx.BeginDrag(Wui::HashId(("browser.drag." + rel.string()).c_str()), "file:" + rel.string());
				ctx.SetCursor(Wui::WuiCursor::Hand);
			}
			if (ctx.IsDoubleClicked(itemRect))
				BrowserOpenItem(path);
			else if (ctx.IsClicked(itemRect))
			{
				if (ctx.Input().Ctrl)
				{
					if (selected) m_Browser.Selected.erase(path);
					else m_Browser.Selected.insert(path);
				}
				else if (ctx.Input().Shift && !m_Browser.LastSelected.empty())
				{
					auto start = std::find(paths.begin(), paths.end(), m_Browser.LastSelected);
					auto end = std::find(paths.begin(), paths.end(), path);
					if (start != paths.end() && end != paths.end())
					{
						if (std::distance(start, end) < 0) std::swap(start, end);
						for (auto it = start; it <= end; ++it)
							m_Browser.Selected.insert(*it);
					}
				}
				else
				{
					m_Browser.Selected.clear();
					m_Browser.Selected.insert(path);
				}
				m_Browser.LastSelected = path;
			}
			else if (ctx.Input().MouseClicked[1] && hovered)
			{
				if (!selected)
				{
					m_Browser.Selected.clear();
					m_Browser.Selected.insert(path);
					m_Browser.LastSelected = path;
				}
				contextPath = path;
				ctx.OpenPopup(Wui::HashId("browser.context"));
			}
			return selected || hovered;
		};

		if (m_Browser.ListMode)
		{
			const float rowH = 24;
			Label(ctx, { content.X + 8, content.Y + 4 }, "Name", m_Theme.TextMuted, 13.0f);
			Label(ctx, { content.X + content.W * 0.52f, content.Y + 4 }, "Type", m_Theme.TextMuted, 13.0f);
			Label(ctx, { content.X + content.W * 0.72f, content.Y + 4 }, "Size", m_Theme.TextMuted, 13.0f);
			BeginScrollArea(ctx, { content.X, content.Y + 22, content.W, content.H - 22 }, paths.size() * rowH, m_Browser.ContentScroll, m_Theme);
			for (size_t i = 0; i < paths.size(); ++i)
			{
				const std::filesystem::path& path = paths[i];
				std::error_code dirError;
				const bool isDir = std::filesystem::is_directory(path, dirError);
				const Wui::WuiRect row { content.X + 4, content.Y + 24 + i * rowH - m_Browser.ContentScroll, content.W - 8, rowH };
				interact(path, row, isDir);
				if (m_Browser.Selected.find(path) != m_Browser.Selected.end())
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, { 0.28f, 0.45f, 0.85f, 0.35f }, 2.0f });
				Ref<Texture2D> icon = isDir ? m_Browser.DirIcon : m_Browser.FileIcon;
				if (icon)
					Image(ctx, { row.X + 2, row.Y + 3, 18, 18 }, icon->GetRendererID(), { 0, 1, 1, -1 }, m_Theme);
				Label(ctx, { row.X + 26, row.Y + 4 }, path.filename().string(), m_Theme.Text, 13.0f);
				Label(ctx, { row.X + content.W * 0.52f, row.Y + 4 }, isDir ? "Folder" : "File", m_Theme.TextMuted, 13.0f);
				std::string size = "-";
				if (!isDir)
					size = FormatBytes(static_cast<size_t>(BrowserFileSize(path)));
				Label(ctx, { row.X + content.W * 0.72f, row.Y + 4 }, size, m_Theme.TextMuted, 13.0f);
				if (m_Browser.RenameTarget == path)
				{
					std::string renameText = m_Browser.RenameBuffer;
					if (TextField(ctx, Wui::HashId("browser.rename"), { row.X + 26, row.Y + 2, 160, 20 }, renameText, m_Theme))
						BrowserApplyRename(path, renameText);
					else if (ctx.IsKeyPressed(KeyCodes::Escape))
						m_Browser.RenameTarget.clear();
				}
			}
			EndScrollArea(ctx);
		}
		else
		{
			const float cell = 142;
			const int columns = std::max(1, static_cast<int>(content.W / cell));
			const float rows = std::ceil(static_cast<float>(paths.size()) / columns);
			BeginScrollArea(ctx, content, rows * cell + 16, m_Browser.ContentScroll, m_Theme);
			for (size_t i = 0; i < paths.size(); ++i)
			{
				const int column = static_cast<int>(i % columns);
				const int row = static_cast<int>(i / columns);
				const Wui::WuiRect cellRect { content.X + 8 + column * cell, content.Y + 8 + row * cell - m_Browser.ContentScroll, 128, 128 };
				const std::filesystem::path& path = paths[i];
				std::error_code dirError;
				const bool isDir = std::filesystem::is_directory(path, dirError);
				const bool selected = m_Browser.Selected.find(path) != m_Browser.Selected.end();
				const bool hovered = ctx.IsHovered(cellRect);
				if (selected)
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { cellRect.X - 3, cellRect.Y - 3, cellRect.W + 6, cellRect.H + 28 }, { 0.28f, 0.45f, 0.85f, 0.35f }, 4.0f });
				else if (hovered)
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { cellRect.X - 3, cellRect.Y - 3, cellRect.W + 6, cellRect.H + 28 }, m_Theme.ButtonHover, 4.0f });
				interact(path, cellRect, isDir);
				Ref<Texture2D> icon = isDir ? m_Browser.DirIcon : m_Browser.FileIcon;
				if (icon)
					Image(ctx, cellRect, icon->GetRendererID(), { 0, 1, 1, -1 }, m_Theme);
				Label(ctx, { cellRect.X, cellRect.Y + 130 }, path.filename().string(), m_Theme.Text, 13.0f);
				if (m_Browser.RenameTarget == path)
				{
					std::string renameText = m_Browser.RenameBuffer;
					if (TextField(ctx, Wui::HashId("browser.rename"), { cellRect.X, cellRect.Y + 150, 128, 22 }, renameText, m_Theme))
						BrowserApplyRename(path, renameText);
					else if (ctx.IsKeyPressed(KeyCodes::Escape))
						m_Browser.RenameTarget.clear();
				}
			}
			EndScrollArea(ctx);
		}

		// 落位处理:AcceptDrop 在松开鼠标后的下一帧才返回 true,
		// 因此目标目录必须跨帧保持(PendingDropDest),且不能在
		// 消费前因 "drag 已结束" 被提前清空。
		if (ctx.AcceptDrop(&filePayload, "file:"))
		{
			if (!m_Browser.PendingDropDest.empty())
			{
				const std::filesystem::path dragged = m_Browser.Root / filePayload.substr(5);
				const std::filesystem::path dest = m_Browser.PendingDropDest;
				// a == b 或 a 是 b 的祖先目录(禁止把文件夹移进自身子树)。
				auto sameOrAncestor = [](const std::filesystem::path& a, const std::filesystem::path& b)
				{
					const std::filesystem::path na = a.lexically_normal();
					const std::filesystem::path nb = b.lexically_normal();
					auto ia = na.begin();
					auto ib = nb.begin();
					while (ia != na.end() && ib != nb.end() && *ia == *ib)
					{
						++ia;
						++ib;
					}
					return ia == na.end();
				};
				const bool samePath = dragged == dest;
				const bool sameParent = dragged.parent_path() == dest;
				const bool intoOwnSubtree = std::filesystem::is_directory(dragged) && !samePath && sameOrAncestor(dragged, dest);
				if (!samePath && !sameParent && !intoOwnSubtree)
				{
					const std::filesystem::path movedTo = dest / dragged.filename();
					try
					{
						std::filesystem::rename(dragged, movedTo);
						m_Ctx->RecordOp("browser", "move", dragged.filename().string(), "-> " + dest.string());
						if (m_Browser.Selected.erase(dragged) > 0)
							m_Browser.Selected.insert(movedTo);
						if (m_Browser.LastSelected == dragged)
							m_Browser.LastSelected = movedTo;
						if (m_Browser.Current == dragged)
						{
							m_Browser.Current = movedTo;
							if (m_Browser.Search[0])
								UpdateBrowserSearch();
						}
						BrowserReveal(dest);
						InvalidateBrowserContents();
						SaveBrowserState();
					}
					catch (const std::exception& error)
					{
						WLD_CORE_ERROR("Content browser move failed: {0}", error.what());
					}
				}
			}
			m_Browser.PendingDropDest.clear();
		}
		else if (!fileDrag)
		{
			// 拖拽已结束且没有落位消费:清掉陈旧目标,避免影响下一次拖拽。
			m_Browser.PendingDropDest.clear();
		}

		// ---- 右键菜单 ----
		const Wui::WuiId popup = Wui::HashId("browser.context");
		if (ctx.IsPopupOpen(popup) && !contextPath.empty())
		{
			ctx.PushOverlay();
			const Wui::WuiRect menuPanel { ctx.Input().MousePos.x, ctx.Input().MousePos.y, 180, 8 * 24 + 8 };
			DrawPanelSurface(ctx, menuPanel, m_Theme);
			struct BrowserItem { const char* Label; std::function<void()> Action; };
			const bool single = m_Browser.Selected.size() == 1;
			const std::vector<BrowserItem> items = {
				{ "Open", [this, &contextPath] { BrowserOpenItem(contextPath); } },
				{ "Cut", [this] { BrowserCut(); } },
				{ "Copy", [this] { BrowserCopy(); } },
				{ "Paste", [this, &contextPath] { BrowserPasteInto(std::filesystem::is_directory(contextPath) ? contextPath : m_Browser.Current); } },
				{ "Rename", [this, &contextPath, single] { if (single) { m_Browser.RenameTarget = contextPath; std::strncpy(m_Browser.RenameBuffer, contextPath.filename().string().c_str(), sizeof(m_Browser.RenameBuffer) - 1); } } },
				{ "New Folder", [this] { BrowserCreateFolder(); } },
				{ "Open in Explorer", [this, &contextPath] { const std::string cmd = "explorer \"" + std::filesystem::absolute(contextPath).string() + "\""; system(cmd.c_str()); } },
				{ "Delete", [this] { m_Browser.ShowDeleteModal = true; } },
			};
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Wui::WuiRect item { menuPanel.X + 4, menuPanel.Y + 4 + i * 24, menuPanel.W - 8, 22 };
				if (MenuItem(ctx, Wui::HashId(("browser.item." + std::string(items[i].Label)).c_str()), item, items[i].Label, true, m_Theme))
				{
					items[i].Action();
					m_Ctx->RecordOp("menu", "item", items[i].Label, "browser");
					ctx.CloseAllPopups();
				}
			}
			ctx.ClosePopupsOnOutsideClick({ popup }, menuPanel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(popup);
			ctx.PopOverlay();
		}

		// ---- 删除确认 ----
		const Wui::WuiId deleteModal = Wui::HashId("browser.delete");
		if (m_Browser.ShowDeleteModal)
		{
			ctx.SetModal(deleteModal);
			m_Browser.ShowDeleteModal = false;
		}
		else if (ctx.Modal() == deleteModal)
			ctx.ClearModal();
		Wui::WuiRect panel;
		if (BeginModal(ctx, deleteModal, "Delete Confirmation", { 380, 160 }, &panel, m_Theme))
		{
			Label(ctx, { panel.X + 16, panel.Y + 48 }, "Delete " + std::to_string(m_Browser.Selected.size()) + " item(s)? This cannot be undone.", m_Theme.Text, 14.0f);
			if (Button(ctx, Wui::HashId("browser.delete.yes"), { panel.X + 20, panel.Y + 110, 110, 28 }, "Yes", m_Theme))
			{
				BrowserDeleteSelection();
				ctx.ClearModal();
			}
			if (Button(ctx, Wui::HashId("browser.delete.no"), { panel.X + 150, panel.Y + 110, 110, 28 }, "No", m_Theme))
				ctx.ClearModal();
			EndModal(ctx, deleteModal);
		}
	}
	void EditorShell::DrawModals(Wui::WuiContext& ctx)
	{
		const Wui::WuiId unsaved = Wui::HashId("modal.unsaved");
		if (m_Editor.ShowUnsavedModal()) ctx.SetModal(unsaved);
		else if (ctx.Modal() == unsaved) ctx.ClearModal();
		Wui::WuiRect panel;
		if (BeginModal(ctx, unsaved, "Unsaved Changes", { 460, 150 }, &panel, m_Theme))
		{
			Label(ctx, { panel.X + 16, panel.Y + 48 }, "The current scene has unsaved changes.", m_Theme.Text, 14.0f);
			if (Button(ctx, Wui::HashId("modal.unsaved.save"), { panel.X + 20, panel.Y + 100, 110, 28 }, "Save", m_Theme))
			{
				m_Editor.ResolveUnsavedModal(true);
				ctx.ClearModal();
			}
			if (Button(ctx, Wui::HashId("modal.unsaved.nosave"), { panel.X + 145, panel.Y + 100, 120, 28 }, "Don't Save", m_Theme))
			{
				m_Editor.ResolveUnsavedModal(false);
				ctx.ClearModal();
			}
			if (Button(ctx, Wui::HashId("modal.unsaved.cancel"), { panel.X + 280, panel.Y + 100, 100, 28 }, "Cancel", m_Theme))
			{
				m_Editor.CancelUnsavedModal();
				ctx.ClearModal();
			}
			EndModal(ctx, unsaved);
		}

		const Wui::WuiId error = Wui::HashId("modal.error");
		if (m_Editor.ShowErrorModal()) ctx.SetModal(error);
		else if (ctx.Modal() == error) ctx.ClearModal();
		if (BeginModal(ctx, error, "Error", { 460, 150 }, &panel, m_Theme))
		{
			Label(ctx, { panel.X + 16, panel.Y + 48 }, m_Editor.ErrorText(), m_Theme.Text, 14.0f);
			if (Button(ctx, Wui::HashId("modal.error.ok"), { panel.X + 180, panel.Y + 100, 100, 28 }, "OK", m_Theme))
			{
				m_Editor.ShowErrorModal() = false;
				m_Editor.ErrorText().clear();
				ctx.ClearModal();
			}
			EndModal(ctx, error);
		}

		const Wui::WuiId cooking = Wui::HashId("modal.cooking");
		if (m_Editor.ShowCookingProgress()) ctx.SetModal(cooking);
		else if (ctx.Modal() == cooking) ctx.ClearModal();
		if (BeginModal(ctx, cooking, "Packaging", { 420, 140 }, &panel, m_Theme))
		{
			if (m_Editor.CookingFinished())
			{
				const std::string message = m_Editor.CookingSucceeded() ? "Packaging finished." : "Packaging failed: " + m_Editor.CookingError();
				Label(ctx, { panel.X + 16, panel.Y + 48 }, message, m_Theme.Text, 14.0f);
				if (Button(ctx, Wui::HashId("modal.cooking.ok"), { panel.X + 160, panel.Y + 95, 100, 28 }, "OK", m_Theme))
				{
					m_Editor.ShowCookingProgress() = false;
					ctx.ClearModal();
				}
			}
			else
			{
				Label(ctx, { panel.X + 16, panel.Y + 52 }, "Packaging...", m_Theme.Text, 14.0f);
			}
			EndModal(ctx, cooking);
		}
	}
}
