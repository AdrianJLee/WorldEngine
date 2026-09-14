#include "wldpch.h"
#include "EditorShell.h"
#include "../EditorLayer.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/KeyCodes.h"
#include "World/Renderer/Renderer.h"
#include "World/WUI/WuiLayoutStore.h"
#include "World/WUI/WuiWidgets.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <tuple>

namespace World
{
	namespace
	{
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
		: m_Editor(editor), m_LayoutPath(std::string(WLD_EDITOR_DIR) + "wui-layout.json")
	{
		// 注意:"attach_slot" 暂不在默认面板列表/Window 菜单中暴露:
		// W7.1 初版在"槽位面板存在 + 独立窗口创建"组合下会崩溃(已定位到槽位面板路径),
		// 修复后再放回列表。面板实现与挂靠逻辑保留,便于继续排查。
		const std::vector<Wui::PanelId> panels = { "hierarchy", "properties", "content_browser", "view", "gallery", "windows", "stats", "memory", "operations" };
		m_Panels = panels;
		const Wui::DockLayout fallback = Wui::DockLayout::Default(panels);
		std::string error;
		if (!Wui::WuiLayoutStore::Load(m_LayoutPath, fallback, &m_Layout, &error))
			WLD_CORE_WARN("Failed to load WUI layout, using default: {0}", error);

		// 尊重用户关闭的面板:仅当布局文件缺失/损坏时使用默认布局,
		// 不把"已关闭"的面板强制补回。重新显示由 Window 菜单负责。

		m_PanelRegistry.emplace("hierarchy", std::make_unique<HierarchyPanel>());
		m_PanelRegistry.emplace("properties", std::make_unique<PropertiesPanel>(*this));
		m_PanelRegistry.emplace("content_browser", std::make_unique<ContentBrowserPanel>(*this));
		m_PanelRegistry.emplace("view", std::make_unique<ViewportPanel>(*this));
		m_PanelRegistry.emplace("stats", std::make_unique<StatsPanel>());
		m_PanelRegistry.emplace("memory", std::make_unique<MemoryPanel>());
		m_PanelRegistry.emplace("operations", std::make_unique<OperationsPanel>());
		m_PanelRegistry.emplace("gallery", std::make_unique<WidgetGalleryPanel>());
		m_PanelRegistry.emplace("windows", std::make_unique<WindowsPanel>());
		// 独立窗口(与停靠面板是不同组件):按屏幕矩形分组重建,
		// 同一窗口的多个标签共享一个容器(多标签窗口)。
		std::map<std::tuple<int, int, int, int>, std::vector<std::string>> floatGroups;
		for (const Wui::DockFloat& entry : m_Layout.Floating)
		{
			const auto key = std::make_tuple(
				static_cast<int>(entry.Rect.X), static_cast<int>(entry.Rect.Y),
				static_cast<int>(entry.Rect.W), static_cast<int>(entry.Rect.H));
			floatGroups[key].push_back(entry.Panel);
		}
		for (const auto& [key, panels] : floatGroups)
		{
			if (panels.empty())
				continue;
			const Wui::WuiRect rect { static_cast<float>(std::get<0>(key)), static_cast<float>(std::get<1>(key)),
				static_cast<float>(std::get<2>(key)), static_cast<float>(std::get<3>(key)) };
			AddFloatWindow(panels.front(), rect, "restore");
			if (FloatWindowHost* host = m_FloatHosts.empty() ? nullptr : m_FloatHosts.back().get())
			{
				for (size_t i = 1; i < panels.size(); ++i)
					host->AddPanel(panels[i], false);
			}
		}
		// 跨会话记忆:曾经作为独立窗口存在过的面板,其屏幕矩形用于下次打开。
		for (const Wui::DockFloat& entry : m_Layout.FloatMemory)
			m_LastFloatRects[entry.Panel] = entry.Rect;
	}

	EditorShell::~EditorShell() = default;

	// ---- PanelHost ----

	Ref<Scene> EditorShell::GetActiveScene()
	{
		return m_Editor.GetActiveScene();
	}

	Entity EditorShell::GetSelectedEntity()
	{
		return m_Editor.GetSelectedEntity();
	}

	void EditorShell::SetSelectedEntity(Entity entity)
	{
		m_Editor.SetSelectedEntity(entity);
	}

	void EditorShell::MarkDocumentDirty()
	{
		m_Editor.MarkDocumentDirty();
	}

	void EditorShell::DuplicateSelectedEntity()
	{
		m_Editor.DuplicateSelectedEntity();
	}

	void EditorShell::OpenScene(const std::filesystem::path& path)
	{
		m_Editor.OpenScene(path);
	}

	// ---- ViewportHost ----

	bool EditorShell::HasRenderedScene() const
	{
		return m_Editor.HasRenderedScene();
	}

	Ref<SceneRenderer>& EditorShell::GetSceneRenderer()
	{
		return m_Editor.GetSceneRenderer();
	}

	void EditorShell::SetViewportState(bool focused, bool hovered, glm::vec2 size, glm::vec2 bounds[2])
	{
		m_Editor.SetViewportState(focused, hovered, size, bounds);
	}

	void EditorShell::SetViewportRect(const Wui::WuiRect& rect)
	{
		m_ViewportRect = rect;
	}

	bool EditorShell::IsPlaying() const { return m_Editor.IsPlaying(); }
	bool EditorShell::IsSimulating() const { return m_Editor.IsSimulating(); }
	bool EditorShell::IsPaused() const { return m_Editor.IsPaused(); }
	void EditorShell::TogglePlay() { m_Editor.TogglePlay(); }
	void EditorShell::ToggleSimulate() { m_Editor.ToggleSimulate(); }
	void EditorShell::TogglePause() { m_Editor.TogglePause(); }

	Ref<Texture2D> EditorShell::GetIcon(int index) const
	{
		return m_Editor.GetIcon(index);
	}

	uint64_t EditorShell::GetIconId(int index) const
	{
		return m_Editor.GetIconId(index);
	}

	uint64_t EditorShell::GetSceneTextureId() const
	{
		return m_Editor.GetSceneTextureId();
	}

	Entity EditorShell::PickEntityAt(glm::vec2 viewportLocal)
	{
		return m_Editor.PickEntityAt(viewportLocal);
	}

	EditorCamera& EditorShell::GetEditorCamera()
	{
		return m_Editor.GetEditorCamera();
	}

	Wui::GizmoOperation EditorShell::GetGizmoOperation() const
	{
		return m_Editor.GetGizmoOperation();
	}

	const char* EditorShell::PanelTitle(const std::string& id) const
	{
		const auto it = m_PanelRegistry.find(id);
		return it != m_PanelRegistry.end() ? it->second->Title() : id.c_str();
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
		if (m_Layout.IsFloating(panel))
		{
			// 隐藏独立窗口内的该面板:它只是窗口的一个标签,窗口可继续承载其它面板。
			HideFloatPanel(panel, &ctx);
			return;
		}
		if (m_Layout.Contains(panel))
		{
			if (m_Layout.RemoveTab(panel))
				RecordDockChange(ctx, "hide", panel, before);
		}
		else
		{
			// 只有可浮动的面板(Widget 画廊)从 Window 菜单打开时以独立窗口出现;
			// 其余面板回到停靠树。
			if (!IsFloatablePanel(panel))
			{
				const std::string anchor = m_Layout.FirstPanel();
				if (anchor.empty())
				{
					Wui::DockLayout fresh;
					fresh.Root.Panels.push_back(panel);
					m_Layout = std::move(fresh);
					RecordDockChange(ctx, "show", panel, before);
				}
				else if (m_Layout.AddTab(panel, anchor, Wui::DropZone::Center))
					RecordDockChange(ctx, "show", panel, before);
				return;
			}
			Wui::WuiRect rect;
			const auto remembered = m_LastFloatRects.find(panel);
			if (remembered != m_LastFloatRects.end())
				rect = remembered->second;
			if (rect.W < 200.0f || rect.H < 140.0f)
			{
				int windowX = 0, windowY = 0;
				if (Application::HasInstance())
					Application::Get().GetWindow().GetPosition(&windowX, &windowY);
				rect = { static_cast<float>(windowX) + 140.0f, static_cast<float>(windowY) + 100.0f, 520.0f, 400.0f };
			}
			// 该面板此刻既不在停靠树也不在浮动列表(Float 只接受已停靠面板),
			// 直接登记浮动记录再创建窗口。
			m_Layout.Floating.push_back({ panel, rect });
			AddFloatWindow(panel, rect, "reopen");
			RecordDockChange(ctx, "float", panel, before);
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
		// 编辑器级四边停靠区:拖拽面板进入窗口边缘条带时,生成横跨整个编辑器的
		// 停靠区(而不是只切分鼠标所在的面板组)。需在渲染面板前判定,以便
		// RenderTabs 跳过面板内的落区逻辑。
		const float editorTop = 26.0f + m_AttachBarHeight;
		const Wui::WuiRect editorArea { 0, editorTop, viewport.x, viewport.y - editorTop };
		std::string edgePayload;
		const bool edgeDragActive = ctx.IsDragActive(&edgePayload) && edgePayload.rfind("panel:", 0) == 0;
		if (edgeDragActive)
		{
			m_EdgeDockActive = false;
			m_EdgeDropZone = Wui::DropZone::Center;
			if (ctx.IsHovered(editorArea))
			{
				constexpr float edgeBand = 110.0f;
				const glm::vec2 mouse = ctx.Input().MousePos;
				if (mouse.x - editorArea.X <= edgeBand) m_EdgeDropZone = Wui::DropZone::Left;
				else if (editorArea.X + editorArea.W - mouse.x <= edgeBand) m_EdgeDropZone = Wui::DropZone::Right;
				else if (mouse.y - editorArea.Y <= edgeBand) m_EdgeDropZone = Wui::DropZone::Top;
				else if (editorArea.Y + editorArea.H - mouse.y <= edgeBand) m_EdgeDropZone = Wui::DropZone::Bottom;
				m_EdgeDockActive = m_EdgeDropZone != Wui::DropZone::Center;
				if (m_EdgeDockActive)
				{
					ctx.DropTarget(editorArea, "panel:");
					// 原面板落区状态让位,避免同时高亮/同时落位。
					m_DropTargetPanel.clear();
					m_DropZone = Wui::DropZone::Center;
				}
			}
		}
		// 拖拽结束的落位在下一帧消费,因此拖拽不活跃时保留上一帧的边缘落区状态。
		RenderNode(ctx, m_Layout.Root, editorArea);

		// 边缘落位提示必须在面板之后绘制:面板背景是不透明矩形,
		// 先画会被完全遮住(表现就是"没有目标位置提示")。
		if (m_EdgeDockActive)
		{
			Wui::WuiRect zone = editorArea;
			if (m_EdgeDropZone == Wui::DropZone::Left) zone.W = editorArea.W * 0.25f;
			else if (m_EdgeDropZone == Wui::DropZone::Right) { zone.X = editorArea.X + editorArea.W * 0.75f; zone.W = editorArea.W * 0.25f; }
			else if (m_EdgeDropZone == Wui::DropZone::Top) zone.H = editorArea.H * 0.25f;
			else if (m_EdgeDropZone == Wui::DropZone::Bottom) { zone.Y = editorArea.Y + editorArea.H * 0.75f; zone.H = editorArea.H * 0.25f; }
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, zone, { 0.3f, 0.5f, 0.9f, 0.30f }, 3.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, zone, { 0.45f, 0.65f, 1.0f, 1.0f }, 3.0f, 2.0f });
		}

		std::string payload;
		bool dropConsumed = false;
		if (ctx.AcceptDrop(&payload, "panel:"))
		{
			dropConsumed = true;
			if (payload.rfind("panel:", 0) == 0)
			{
				const std::string panel = payload.substr(6);
				const bool floating = m_Layout.IsFloating(panel);
				if (m_EdgeDockActive && (floating || m_Layout.Contains(panel)))
				{
					const std::string before = m_Layout.Serialize();
					const bool docked = floating
						? m_Layout.DockFloatingToRoot(panel, m_EdgeDropZone)
						: m_Layout.DockToRoot(panel, m_EdgeDropZone);
					if (docked)
						RecordDockChange(ctx, "drop", panel + " -> editor/" + ZoneName(m_EdgeDropZone), before);
				}
				else if (!m_DropTargetPanel.empty() && (floating || m_Layout.Contains(panel)))
				{
					const std::string before = m_Layout.Serialize();
					const bool docked = floating
						? m_Layout.DockFloating(panel, m_DropTargetPanel, m_DropZone)
						: m_Layout.MoveTab(panel, m_DropTargetPanel, m_DropZone);
					if (docked)
						RecordDockChange(ctx, "drop", panel + " -> " + m_DropTargetPanel + "/" + ZoneName(m_DropZone), before);
				}
			}
			// 只在落位消费后清空,避免下一帧 AcceptDrop 读取时目标已被清。
			m_DropTargetPanel.clear();
			m_DropZone = Wui::DropZone::Center;
			m_EdgeDockActive = false;
			m_EdgeDropZone = Wui::DropZone::Center;
			m_MovingFloat.clear();
		}

		// ---- 面板拖拽状态机:停靠面板一旦进入拖拽即"拖出"为浮动窗口 ----
		// 拖到落点上释放会重新停靠(DockFloating*),否则保持浮动并跟随鼠标。
		std::string activePayload;
		if (ctx.IsDragActive(&activePayload) && activePayload.rfind("panel:", 0) == 0)
		{
			m_DragPanel = activePayload.substr(6);
			m_LastDragPos = ctx.Input().MousePos;
			// 只有"本次拖拽确实起手于该面板的标签页"时才允许拖出为独立窗口;
			// 否则残留的拖拽状态会在挂靠后立刻把面板再次浮出(表现为多出一个窗口)。
			if (!dropConsumed && m_AttachCooldownFrames <= 0 && m_Layout.Contains(m_DragPanel)
				&& m_TabDragPanel == m_DragPanel && IsFloatablePanel(m_DragPanel))
			{
				// 拖出即成为独立 OS 窗口:按原停靠尺寸创建,放在鼠标所在的屏幕位置。
				Wui::WuiRect source { m_LastDragPos.x - 40.0f, m_LastDragPos.y - 12.0f, 480.0f, 320.0f };
				if (const auto remembered = m_LastFloatRects.find(m_DragPanel); remembered != m_LastFloatRects.end())
				{
					source.W = remembered->second.W;
					source.H = remembered->second.H;
				}
				// 屏幕坐标 = 主窗口位置 + 客户区鼠标位置。
				int windowX = 0, windowY = 0;
				if (Application::HasInstance())
					Application::Get().GetWindow().GetPosition(&windowX, &windowY);
				source.X += static_cast<float>(windowX);
				source.Y += static_cast<float>(windowY);
				std::vector<std::pair<Wui::PanelId, Wui::WuiRect>> rects;
				if (m_LastFloatRects.find(m_DragPanel) == m_LastFloatRects.end())
				{
					// 首次拖出:沿用原停靠区的尺寸作为初始浮动尺寸。
					m_Layout.ComputeRects({ 0, 26, viewport.x, viewport.y - 26 }, &rects);
					for (const auto& entry : rects)
					{
						if (entry.first != m_DragPanel)
							continue;
						source.W = std::max(360.0f, entry.second.W * 0.75f);
						source.H = std::max(260.0f, entry.second.H * 0.75f);
						break;
					}
				}
				const std::string before = m_Layout.Serialize();
				if (m_Layout.Float(m_DragPanel, source))
				{
					RecordDockChange(ctx, "float", m_DragPanel, before);
					AddFloatWindow(m_DragPanel, source, "drag-out");
				}
			}
			else if (!m_DragPanel.empty() && m_Layout.Contains(m_DragPanel))
			{
				// 拖出条件未满足:保持原状(用于人工排查,不打印高频日志)。
			}
		}
		else if (ctx.IsDragActive(nullptr))
		{
			// 其他类型的拖拽(file: 等)不参与面板浮动。
			m_DragPanel.clear();
		}
		else if (!m_DragPanel.empty())
		{
			// 拖拽结束:落点已在上方处理;未回收则保持浮动位置。
			m_DragPanel.clear();
			m_MovingFloat.clear();
		}

		// 浮动面板绘制在停靠区之上、菜单/模态之下。
		RenderFloating(ctx);

		// 菜单栏最后绘制:其弹出面板需要盖在所有停靠面板之上。
		DrawMenuBar(ctx);
		// 挂靠栏:横条形式(排在菜单栏下方),独立窗口可挂靠至此。
		DrawAttachBar(ctx);

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
			if (ctx.Input().MouseDown[0] && ctx.IsHovered(tab))
				m_TabDragPanel = panel; // 记录本次拖拽的真实来源

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
		if (!m_EdgeDockActive && ctx.IsDragActive(&dragPayload) && dragPayload.rfind("panel:", 0) == 0 && ctx.IsHovered(area))
		{
			const glm::vec2 rel = ctx.Input().MousePos - glm::vec2 { area.X, area.Y };
			const float lx = area.W > 0 ? rel.x / area.W : 0;
			const float ly = area.H > 0 ? rel.y / area.H : 0;
			Wui::DropZone targetZone = Wui::DropZone::Center;
			if (lx < 0.25f) targetZone = Wui::DropZone::Left;
			else if (lx > 0.75f) targetZone = Wui::DropZone::Right;
			else if (ly < 0.25f) targetZone = Wui::DropZone::Top;
			else if (ly > 0.75f) targetZone = Wui::DropZone::Bottom;
			// 浮动窗口只在"明确落区"上停靠:四边 25% 条带或 tab 栏(Center);
			// 停在面板中部不会把浮动窗口吸回停靠,便于自由摆放。
			const bool draggingFloating = m_Layout.IsFloating(dragPayload.substr(6));
			const bool overTabBar = ctx.IsHovered({ area.X, area.Y, area.W, tabH });
			if (draggingFloating && targetZone == Wui::DropZone::Center && !overTabBar)
			{
				m_DropTargetPanel.clear();
				return;
			}

			ctx.DropTarget(area, "panel:"); // 武装落点:仅面板拖拽在此生效
			m_DropZone = targetZone;
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
		const auto it = m_PanelRegistry.find(id);
		if (it != m_PanelRegistry.end())
			it->second->OnRender(ctx, rect, *this);
		else
			Label(ctx, { rect.X + 8, rect.Y + 8 }, PanelTitle(id), m_Theme.TextMuted, 14.0f);
	}

	// 挂靠栏:横跨主窗口的一条,按"窗口"列出一行(chip 显示活动标签名与标签数),
	// 独立窗口被拖到这条栏上(窗口中心进入栏内并停稳)或点 Attach 会整窗挂靠回主窗口。
	void EditorShell::DrawAttachBar(Wui::WuiContext& ctx)
	{
		const glm::vec2 viewport = ctx.ViewportSize();
		// 顶部第一行即挂靠栏:也是无边框主窗口的拖动/关闭区域。
		const Wui::WuiRect bar { 0, 0.0f, viewport.x, m_AttachBarHeight };
		const Wui::WuiColor fill = m_AttachSlotHighlight
			? Wui::WuiColor { 0.3f, 0.5f, 0.9f, 0.45f }
			: m_Theme.PanelHeader;
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, bar, fill, 0.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { bar.X, bar.Y + bar.H - 1.0f, bar.W, 1.0f }, m_Theme.Border, 0.0f });

		// 最右侧:窗口关闭(主窗口)与空区拖动。
		const Wui::WuiRect windowClose { bar.X + bar.W - 20.0f, bar.Y + 5.0f, 14.0f, 14.0f };
		if (ctx.IsHovered(windowClose))
		{
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, windowClose, m_Theme.ButtonHover, 2.0f });
			ctx.SetCursor(Wui::WuiCursor::Hand);
		}
		ctx.Commands().push_back({ Wui::WuiDrawKind::Text, { windowClose.X + 3.0f, windowClose.Y - 1.0f, 0, 0 },
			m_Theme.TextMuted, 0, 1.0f, "x", 13.0f, false });
		if (ctx.IsClicked(windowClose))
		{
			m_Editor.CloseAction();
			return;
		}

		if (m_FloatHosts.empty())
		{
			Label(ctx, { bar.X + 10, bar.Y + 5 }, "挂靠栏:把独立窗口拖到这条栏上即可挂靠回主窗口", m_Theme.TextMuted, 13.0f);
			if (ctx.Input().MouseDown[0] && ctx.IsHovered(bar))
				Application::Get().GetWindow().BeginSystemDrag();
			return;
		}

		float x = bar.X + 8.0f;
		// 注意:挂靠会销毁宿主窗口并从 m_FloatHosts 移除元素,
		// 因此先收集请求,遍历结束后再执行(遍历中修改容器会导致崩溃)。
		std::string attachRequest;
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			// 每行一个窗口:代表面板 = 第一个标签(稳定,不随活动标签切换而变)。
			const std::string panel = host->Panels().empty() ? std::string() : host->Panels().front();
			std::string label = PanelTitle(panel);
			if (host->Panels().size() > 1)
				label += " +" + std::to_string(host->Panels().size() - 1);
			const Wui::WuiRect chip { x, bar.Y + 3.0f, 150.0f, bar.H - 6.0f };
			if (ctx.IsHovered(chip))
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, chip, m_Theme.ButtonHover, 3.0f });
			ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, chip, m_Theme.Border, 3.0f, 1.0f });
			Label(ctx, { chip.X + 8, chip.Y + 3 }, label, m_Theme.Text, 13.0f);
			const Wui::WuiRect attach { chip.X + chip.W + 4.0f, chip.Y, 62.0f, chip.H };
			if (Wui::Button(ctx, Wui::HashId(("attachbar." + panel).c_str()), attach, "Attach", m_Theme))
				attachRequest = panel;
			x += chip.W + 70.0f;
		}
		if (!attachRequest.empty())
			AttachIndependentWindowToSlot(attachRequest);
		else if (ctx.Input().MouseDown[0] && ctx.IsHovered(bar) && !ctx.IsHovered({ 0, 0, x + 70.0f, bar.H }))
			Application::Get().GetWindow().BeginSystemDrag();
	}

	// 独立窗口:每个宿主 = 一个 OS 窗口(可含多个标签面板),绘制在停靠区之上。
	void EditorShell::RenderFloating(Wui::WuiContext& ctx)
	{
		if (m_AttachCooldownFrames > 0)
			--m_AttachCooldownFrames;

		// 槽位屏幕矩形(客户区 -> 屏幕):用于判断独立窗口是否停到了槽位上。
		const glm::vec2 viewport = ctx.ViewportSize();
		std::vector<std::pair<Wui::PanelId, Wui::WuiRect>> dockRects;
		// 挂靠栏的屏幕矩形(横条):客户区坐标 -> 屏幕坐标。
		m_AttachSlotScreenRect = { 0, 0.0f, viewport.x, m_AttachBarHeight };
		int windowX = 0, windowY = 0;
		if (Application::HasInstance())
			Application::Get().GetWindow().GetPosition(&windowX, &windowY);
		m_AttachSlotScreenRect.X += static_cast<float>(windowX);
		m_AttachSlotScreenRect.Y += static_cast<float>(windowY);

		// 每个独立窗口渲染自己的 OS 窗口(含标签栏);窗口被关闭 = 隐藏其全部面板。
		// 标签栏 x 只登记关闭请求,统一在遍历结束后处理,避免边遍历边改 m_FloatHosts。
		std::vector<std::string> closeRequests;
		// 收集新发起的标签拖拽(跨窗口附加);同帧只接受一个。
		if (!m_CrossDragActive)
		{
			for (const std::unique_ptr<FloatWindowHost>& candidate : m_FloatHosts)
			{
				if (const std::string drag = candidate->TakePendingTabDrag(); !drag.empty())
				{
					m_CrossDragActive = true;
					m_CrossDragPanel = drag;
					m_CrossDragSourceKey = candidate->Panels().front();
					m_CrossDragTargetKey.clear();
					POINT cursor { 0, 0 };
					GetCursorPos(&cursor);
					const Wui::WuiRect rect = candidate->ScreenRect();
					m_CrossDragGrab = { static_cast<float>(cursor.x) - rect.X,
						static_cast<float>(cursor.y) - rect.Y };
					break;
				}
			}
		}
		for (size_t i = 0; i < m_FloatHosts.size(); )
		{
			FloatWindowHost& host = *m_FloatHosts[i];
			// 不变量:宿主至少承载一个面板;空宿主属于已关闭窗口,直接移除。
			if (host.Panels().empty())
			{
				EraseFloatHost(&host);
				continue;
			}
			bool alive = true;
			try
			{
				alive = host.Render();
			}
			catch (const std::exception& error)
			{
				WLD_CORE_ERROR("[float] render failed for '{0}': {1}", host.Panel(), error.what());
				alive = false;
			}
			if (!alive)
			{
				const std::string panel = host.Panel();
				CloseFloatWindow(panel, true, &ctx);
				continue; // CloseFloatWindow 会移除该 host
			}
			if (const std::string closing = host.TakeCloseRequest(); !closing.empty())
				closeRequests.push_back(closing);

			// 位置/尺寸变化写回布局(供重启恢复):窗口内每个标签写同一屏幕矩形。
			const Wui::WuiRect rect = host.ScreenRect();
			for (const std::string& panel : host.Panels())
				if (Wui::DockFloat* entry = m_Layout.FindFloat(panel))
					entry->Rect = rect;

			// 挂靠判定:窗口中心停在槽位矩形内且已停稳(位置与上一帧相同)。
			const glm::vec2 center { rect.X + rect.W * 0.5f, rect.Y + rect.H * 0.5f };
			const bool overSlot = m_AttachSlotScreenRect.W > 0.0f && m_AttachSlotScreenRect.H > 0.0f
				&& center.x >= m_AttachSlotScreenRect.X && center.x <= m_AttachSlotScreenRect.X + m_AttachSlotScreenRect.W
				&& center.y >= m_AttachSlotScreenRect.Y && center.y <= m_AttachSlotScreenRect.Y + m_AttachSlotScreenRect.H;
			// 每窗口位置缓存以第一个标签为键(窗口身份 = 面板集合)。
			const std::string windowKey = host.Panels().front();
			const auto previous = m_LastFloatScreenRects.find(windowKey);
			const bool stationary = previous != m_LastFloatScreenRects.end()
				&& std::fabs(previous->second.X - rect.X) < 0.5f && std::fabs(previous->second.Y - rect.Y) < 0.5f;
			m_LastFloatScreenRects[windowKey] = rect;
			if (overSlot)
			{
				m_AttachSlotHighlight = true;
				if (stationary)
				{
					AttachIndependentWindowToSlot(windowKey);
					continue; // host 已销毁
				}
			}
			++i;
		}
		for (const std::string& panel : closeRequests)
			HideFloatPanel(panel, &ctx);
		if (m_FloatHosts.empty())
			m_AttachSlotHighlight = false;
		// 跨窗口拖拽的目标命中与落点(在窗口渲染之后执行,便于统一改容器)。
		UpdateCrossWindowDrag(ctx);
		// 独立窗口渲染会把 GL 上下文切到各自窗口,这里恢复主窗口上下文,
		// 否则主窗口后续的呈现/交换会作用在错误的上下文上(表现为主窗口不再刷新)。
		if (Application::HasInstance())
			Application::Get().GetWindow().MakeCurrent();
	}

	// 跨窗口标签拖拽(浏览器式附加):源窗口标签按下拖动后,这里用全局光标轮询跟踪,
	// 悬停到其他独立窗口标签栏时高亮,松手后按落点执行 附加/新建窗口/挂靠回主窗口。
	void EditorShell::UpdateCrossWindowDrag(Wui::WuiContext& ctx)
	{
		if (!m_CrossDragActive)
			return;
		POINT cursor { 0, 0 };
		GetCursorPos(&cursor);
		const glm::vec2 pos { static_cast<float>(cursor.x), static_cast<float>(cursor.y) };

		// 目标命中:其他独立窗口的标题栏+标签栏区域(顶部约 64px)。
		m_CrossDragTargetKey.clear();
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			const Wui::WuiRect rect = host->ScreenRect();
			const Wui::WuiRect dropZone { rect.X, rect.Y, rect.W, 64.0f };
			const bool hit = host->Panels().front() != m_CrossDragSourceKey
				&& pos.x >= dropZone.X && pos.x <= dropZone.X + dropZone.W
				&& pos.y >= dropZone.Y && pos.y <= dropZone.Y + dropZone.H;
			host->SetTabDropHighlight(hit);
			if (hit)
				m_CrossDragTargetKey = host->Panels().front();
		}

		// 挂靠栏(主窗口)也是有效落点,优先级高于其他独立窗口。
		const bool overAttachBar = m_AttachSlotScreenRect.W > 0.0f
			&& pos.x >= m_AttachSlotScreenRect.X && pos.x <= m_AttachSlotScreenRect.X + m_AttachSlotScreenRect.W
			&& pos.y >= m_AttachSlotScreenRect.Y && pos.y <= m_AttachSlotScreenRect.Y + m_AttachSlotScreenRect.H;
		m_AttachSlotHighlight = overAttachBar;
		if (overAttachBar)
			m_CrossDragTargetKey.clear();

		// 松手(左键释放)才落点。
		if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
			return;

		const std::string panel = m_CrossDragPanel;
		const std::string sourceKey = m_CrossDragSourceKey;
		FloatWindowHost* source = FindFloatHost(panel);
		FloatWindowHost* target = m_CrossDragTargetKey.empty() ? nullptr : FindFloatHost(m_CrossDragTargetKey);

		if (target && source && source != target)
		{
			// 附加到目标窗口:面板从源窗口迁移到目标窗口标签栏。
			source->RemovePanel(panel);
			target->AddPanel(panel, true);
			if (source->Empty())
				EraseFloatHost(source);
			if (Wui::DockFloat* entry = m_Layout.FindFloat(panel))
				entry->Rect = target->ScreenRect();
			ctx.RecordOp("float", "attach", panel, m_CrossDragTargetKey);
		}
		else if (overAttachBar)
		{
			AttachIndependentWindowToSlot(panel);
		}
		else if (source)
		{
			// 桌面空白:若源窗口还有其他标签,拆分为新独立窗口;单标签窗口只算移动。
			if (source->Panels().size() > 1)
			{
				source->RemovePanel(panel);
				const Wui::WuiRect rect { pos.x - m_CrossDragGrab.x, pos.y - m_CrossDragGrab.y, 520.0f, 400.0f };
				AddFloatWindow(panel, rect, "detach");
				if (Wui::DockFloat* entry = m_Layout.FindFloat(panel))
					entry->Rect = rect;
				ctx.RecordOp("float", "detach", panel, "");
			}
		}

		// 清理:高亮与跨窗口拖拽状态全部复位。
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			host->SetTabDropHighlight(false);
		m_CrossDragActive = false;
		m_CrossDragPanel.clear();
		m_CrossDragSourceKey.clear();
		m_CrossDragTargetKey.clear();
		m_AttachSlotHighlight = false;
	}

	void EditorShell::AddFloatWindow(const std::string& panel, const Wui::WuiRect& screenRect, const char* origin)
	{
		WLD_CORE_INFO("[float] AddFloatWindow panel={0} origin={1} rect=({2},{3},{4},{5})",
			panel, origin, screenRect.X, screenRect.Y, screenRect.W, screenRect.H);
		// 独立窗口 = 容器 + 标签栏;标题与内容由面板注册表提供,容器不感知具体面板类型。
		FloatWindowHost::Callbacks callbacks;
		callbacks.Theme = m_Theme;
		callbacks.Title = [this](const std::string& id) { return std::string(PanelTitle(id)); };
		callbacks.Content = [this](Wui::WuiContext& ctx, const Wui::WuiRect& rect, const std::string& id)
		{
			RenderPanelContent(ctx, id, rect);
		};
		try
		{
			m_FloatHosts.push_back(std::make_unique<FloatWindowHost>(panel, PanelTitle(panel), screenRect, std::move(callbacks)));
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("[float] create failed for '{0}': {1}", panel, error.what());
			m_Layout.CloseFloating(panel);
		}
	}

	std::string EditorShell::IndependentWindowPanel(size_t index) const
	{
		// 返回该窗口的代表面板(活动标签);调用方以它定位窗口(焦点/收回/挂靠)。
		return index < m_FloatHosts.size() ? m_FloatHosts[index]->Panel() : std::string();
	}

	std::string EditorShell::IndependentWindowLabel(size_t index) const
	{
		if (index >= m_FloatHosts.size())
			return std::string();
		std::string label;
		for (size_t i = 0; i < m_FloatHosts[index]->Panels().size(); ++i)
		{
			if (i != 0)
				label += " | ";
			label += PanelTitle(m_FloatHosts[index]->Panels()[i]);
		}
		return label;
	}

	FloatWindowHost* EditorShell::FindFloatHost(const std::string& panel)
	{
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (host->Contains(panel))
				return host.get();
		return nullptr;
	}

	void EditorShell::EraseFloatHost(FloatWindowHost* host)
	{
		if (!host)
			return;
		// 每窗口屏幕位置缓存以标签为键:宿主销毁时一并清理,避免重建后误判"停稳"。
		for (const std::string& panel : host->Panels())
			m_LastFloatScreenRects.erase(panel);
		m_FloatHosts.erase(std::remove_if(m_FloatHosts.begin(), m_FloatHosts.end(),
			[&](const std::unique_ptr<FloatWindowHost>& candidate) { return candidate.get() == host; }),
			m_FloatHosts.end());
	}

	void EditorShell::FocusIndependentWindow(const std::string& panel)
	{
		if (FloatWindowHost* host = FindFloatHost(panel))
			host->Focus();
	}

	void EditorShell::DockBackIndependentWindow(const std::string& panel)
	{
		// 收回整窗:记住用户调好的尺寸,销毁独立窗口并把它的全部标签挂回停靠树。
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
			return;
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		for (const std::string& id : panels)
			m_LastFloatRects[id] = rect;
		EraseFloatHost(host);

		const Wui::PanelId anchor = m_Layout.FirstPanel();
		for (const std::string& id : panels)
		{
			// 停靠树为空时无法挂载:仅隐藏该面板(CloseFloating 会留下跨会话位置记忆)。
			if (!anchor.empty() && m_Layout.DockFloating(id, anchor, Wui::DropZone::Center))
				WLD_CORE_INFO("Independent window docked back: {0}", id);
			else
				m_Layout.CloseFloating(id);
		}
	}

	void EditorShell::AttachIndependentWindowToSlot(const std::string& panel)
	{
		// 挂靠整窗:窗口的全部面板作为槽位所在标签组的成员回到主窗口,OS 窗口销毁。
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
			return;
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		for (const std::string& id : panels)
			m_LastFloatRects[id] = rect;
		EraseFloatHost(host);

		const Wui::PanelId slot = "attach_slot";
		const Wui::PanelId anchor = m_Layout.Contains(slot) ? slot : m_Layout.FirstPanel();
		bool placed = false;
		if (anchor.empty())
		{
			// 停靠树已空(所有面板都在独立窗口):整窗面板作为根标签组恢复,
			// 保留 float_memory(跨会话位置记忆)。
			std::vector<Wui::DockFloat> memory = std::move(m_Layout.FloatMemory);
			m_Layout = Wui::DockLayout {};
			m_Layout.FloatMemory = std::move(memory);
			m_Layout.Root.Panels = panels;
			placed = !panels.empty();
		}
		else
		{
			for (const std::string& id : panels)
				placed = m_Layout.DockFloating(id, anchor, Wui::DropZone::Center) || placed;
		}
		if (placed)
		{
			m_AttachSlotHighlight = false;
			// 挂靠后必须清掉拖拽状态:否则同一帧/下一帧的拖拽状态机会把
			// 刚挂靠回停靠树的面板再次"拖出"成独立窗口。
			m_DragPanel.clear();
			m_MovingFloat.clear();
			m_TabDragPanel.clear();
			m_LastDragPos = { 0, 0 };
			m_AttachCooldownFrames = 45;
			WLD_CORE_INFO("Independent window attached to slot: {0} ({1} panels)", panel, panels.size());
		}
	}

	// 隐藏单个面板(标签栏 x / Window 菜单):从所属窗口摘除,窗口为空则销毁。
	void EditorShell::HideFloatPanel(const std::string& panel, Wui::WuiContext* ctx)
	{
		const std::string before = m_Layout.Serialize();
		if (FloatWindowHost* host = FindFloatHost(panel))
		{
			// 写回最后位置:窗口内其余标签仍由它们自己的布局记录继续跟踪。
			const Wui::WuiRect rect = host->ScreenRect();
			if (Wui::DockFloat* entry = m_Layout.FindFloat(panel))
				entry->Rect = rect;
			m_LastFloatRects[panel] = rect;
			host->RemovePanel(panel);
			if (host->Empty())
				EraseFloatHost(host);
		}
		m_LastFloatScreenRects.erase(panel);
		// 宿主不存在时也清掉浮动记录(保持"隐藏即从布局移除"的旧语义)。
		if (m_Layout.CloseFloating(panel) && ctx)
			RecordDockChange(*ctx, "hide", panel, before);
	}

	// 整窗关闭(用户关闭/渲染失败):窗口内全部面板隐藏,布局写回并记录操作。
	void EditorShell::CloseFloatWindow(const std::string& panel, bool recordChange, Wui::WuiContext* ctx)
	{
		const std::string before = m_Layout.Serialize();
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
		{
			// 宿主已不存在:仅清理浮动记录,避免残留"看不见的窗口"。
			if (m_Layout.CloseFloating(panel) && recordChange && ctx)
				RecordDockChange(*ctx, "hide", panel, before);
			return;
		}
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		EraseFloatHost(host);
		bool changed = false;
		for (const std::string& id : panels)
		{
			if (Wui::DockFloat* entry = m_Layout.FindFloat(id))
				entry->Rect = rect;
			m_LastFloatRects[id] = rect;
			changed = m_Layout.CloseFloating(id) || changed;
		}
		if (changed && recordChange && ctx)
			RecordDockChange(*ctx, "hide", panel, before);
	}

	void EditorShell::DrawMenuBar(Wui::WuiContext& ctx)
	{
		const glm::vec2 viewport = ctx.ViewportSize();
		struct MenuEntry { std::string Label; bool Checked; std::function<void()> Action; };

		const Wui::WuiId menuFile = Wui::HashId("menu.file");
		const Wui::WuiId menuWindow = Wui::HashId("menu.window");
		if (!m_MenuBar)
		{
			m_MenuBar = std::make_shared<Wui::WuiBox>();
			m_MenuBar->Direction = Wui::WuiDirection::Row;
			m_MenuBar->Gap = 4;
			m_FileButton = std::make_shared<Wui::WuiButton>();
			m_FileButton->Label = "File";
			m_FileButton->OnClick = [this, menuFile]
				{
					const bool opening = m_OpenMenu != menuFile;
					m_OpenMenu = opening ? menuFile : 0;
					if (opening)
					{
						m_Ctx->CloseAllPopups();
						m_MenuHeaderRect = m_FileButton->Rect();
						m_Ctx->OpenPopup(menuFile);
					}
					else
						m_Ctx->ClosePopup(menuFile);
				};
			m_MenuBar->Add(m_FileButton, { 60, 60, 0, 22, 0 });
			m_WindowButton = std::make_shared<Wui::WuiButton>();
			m_WindowButton->Label = "Window";
			m_WindowButton->OnClick = [this, menuWindow]
				{
					const bool opening = m_OpenMenu != menuWindow;
					m_OpenMenu = opening ? menuWindow : 0;
					if (opening)
					{
						m_Ctx->CloseAllPopups();
						m_MenuHeaderRect = m_WindowButton->Rect();
						m_Ctx->OpenPopup(menuWindow);
					}
					else
						m_Ctx->ClosePopup(menuWindow);
				};
			m_MenuBar->Add(m_WindowButton, { 78, 78, 0, 22, 0 });
		}

		// 菜单栏下移一行:顶部第一行现在是挂靠栏。
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { 0, 26, viewport.x, 26 }, m_Theme.PanelHeader, 0.0f });
		Wui::LayoutWidgetTree(m_MenuBar, { 8, 28, viewport.x - 16, 22 });
		Wui::WuiPaintContext paint(ctx);
		m_MenuBar->Paint(paint);

		auto drawMenu = [&](const Wui::WuiId menuId, const std::string& menuIdName, const std::vector<MenuEntry>& entries)
		{
			if (ctx.IsPopupOpen(menuId))
			{
				ctx.PushOverlay();
				const Wui::WuiRect panel { m_MenuHeaderRect.X, m_MenuHeaderRect.Y + m_MenuHeaderRect.H + 2, 240, static_cast<float>(entries.size() * 22 + 8) };
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
						m_OpenMenu = 0;
					}
				}
				ctx.ClosePopupsOnOutsideClick({ menuId }, panel);
				if (ctx.IsKeyPressed(KeyCodes::Escape))
				{
					ctx.ClosePopup(menuId);
					m_OpenMenu = 0;
				}
				ctx.PopOverlay();
			}
		};

		drawMenu(menuFile, "menu.file", {
			{ "New", false, [this] { m_Editor.NewScene(); } },
			{ "Open", false, [this] { m_Editor.OpenScene(); } },
			{ "Save", false, [this] { m_Editor.SaveScene(); } },
			{ "Project Settings", false, [this]
				{
					std::string error;
					World::Asset::ProjectManifest manifest;
					const std::filesystem::path manifestPath =
						std::string(WLD_GAME_DIR) + "project.we.yaml";
					if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
						m_ProjectRendererIndex = manifest.Renderer == "vulkan" ? 1 : 0;
					else
						WLD_CORE_WARN("Failed to load project manifest: {0}", error);
					m_ShowProjectSettings = true;
				} },
			{ "Generate Lua API Stubs", false, [this] { m_Editor.GenerateLuaStubsAction(); } },
			{ "Cooking", false, [this] { m_Editor.StartCookingAction(); } },
			{ "Export Operation Log", false, [this] { m_Editor.ExportOperationLog(); } },
			{ "Exit", false, [this] { m_Editor.CloseAction(); } },
		});

		std::vector<MenuEntry> windowEntries;
		for (const std::string& panel : m_Panels)
		{
			const bool visible = m_Layout.Contains(panel) || m_Layout.IsFloating(panel);
			windowEntries.push_back({ PanelTitle(panel), visible, [this, panel, &ctx] { TogglePanel(ctx, panel); } });
		}
		windowEntries.push_back({ "Reset Layout", false, [this, &ctx] { ResetLayout(ctx); } });
		drawMenu(menuWindow, "menu.window", windowEntries);
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

		// ---- 项目设置 ----
		const Wui::WuiId projectSettings = Wui::HashId("modal.projectsettings");
		if (m_ShowProjectSettings)
		{
			ctx.SetModal(projectSettings);
			m_ShowProjectSettings = false;
		}
		if (BeginModal(ctx, projectSettings, "Project Settings", { 380, 190 }, &panel, m_Theme))
		{
			Label(ctx, { panel.X + 16, panel.Y + 48 }, "Renderer", m_Theme.TextMuted, 13.0f);
			std::vector<std::string> options = { "OpenGL", "Vulkan" };
			Combo(ctx, Wui::HashId("project.renderer"), { panel.X + 110, panel.Y + 46, 220, 24 },
				"", options, m_ProjectRendererIndex, m_Theme);
			if (Button(ctx, Wui::HashId("project.save"), { panel.X + 60, panel.Y + 125, 110, 28 }, "Save", m_Theme))
			{
				std::string error;
				World::Asset::ProjectManifest manifest;
				const std::filesystem::path manifestPath =
					std::string(WLD_GAME_DIR) + "project.we.yaml";
				if (World::Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
				{
					manifest.Renderer = m_ProjectRendererIndex == 1 ? "vulkan" : "opengl";
					if (World::Asset::ProjectManifest::Save(manifestPath, manifest, &error))
					{
						WLD_CORE_INFO("Project settings saved: renderer={0}", manifest.Renderer);
						m_Editor.ApplyRendererChange(manifest.Renderer);
					}
					else
						WLD_CORE_ERROR("Failed to save project manifest: {0}", error);
				}
				else
					WLD_CORE_ERROR("Failed to load project manifest: {0}", error);
				ctx.ClearModal();
			}
			if (Button(ctx, Wui::HashId("project.cancel"), { panel.X + 190, panel.Y + 125, 110, 28 }, "Cancel", m_Theme))
				ctx.ClearModal();
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClearModal();
			EndModal(ctx, projectSettings);
		}
	}
}
