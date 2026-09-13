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
		const std::vector<Wui::PanelId> panels = { "hierarchy", "properties", "content_browser", "view", "stats", "memory", "operations" };
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

	Entity EditorShell::PickEntityAt(glm::vec2 viewportLocal)
	{
		return m_Editor.PickEntityAt(viewportLocal);
	}

	EditorCamera& EditorShell::GetEditorCamera()
	{
		return m_Editor.GetEditorCamera();
	}

	int EditorShell::GetGizmoOperation() const
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
		const auto it = m_PanelRegistry.find(id);
		if (it != m_PanelRegistry.end())
			it->second->OnRender(ctx, rect, *this);
		else
			Label(ctx, { rect.X + 8, rect.Y + 8 }, PanelTitle(id), m_Theme.TextMuted, 14.0f);
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
			const bool visible = m_Layout.Contains(panel);
			windowEntries.push_back({ PanelTitle(panel), visible, [this, panel, &ctx] { TogglePanel(ctx, panel); } });
		}
		windowEntries.push_back({ "Reset Layout", false, [this, &ctx] { ResetLayout(ctx); } });
		drawMenu("menu.window", "Window", windowEntries);

		EndMenuBar(ctx);
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
