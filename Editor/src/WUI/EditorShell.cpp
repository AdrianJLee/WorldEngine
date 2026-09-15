#include "wldpch.h"
#include "EditorShell.h"
#include "../EditorLayer.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/KeyCodes.h"
#include "World/Renderer/Renderer.h"
#include "World/WUI/WuiLayoutStore.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/Widgets/WuiChrome.h"

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

		// 面板形态声明(单一事实源):Id 同时用于 Window 菜单、面板注册表与布局存档。
		// Independent = "独立窗口"(自带 OS 窗口 + 标签栏;只能挂靠到主窗口顶部挂靠栏)。
		struct PanelSpec
		{
			const char* Id;
			EditorShell::PanelForm Form;
			Wui::WuiRect DefaultFloatRect; // 独立形态:没有位置记忆时的默认屏幕矩形
		};

		const PanelSpec kPanelSpecs[] =
		{
			{ "hierarchy",       EditorShell::PanelForm::Docked, {} },
			{ "properties",      EditorShell::PanelForm::Docked, {} },
			{ "content_browser", EditorShell::PanelForm::Docked, {} },
			{ "view",            EditorShell::PanelForm::Docked, {} },
			{ "stats",           EditorShell::PanelForm::Docked, {} },
			{ "memory",          EditorShell::PanelForm::Docked, {} },
			{ "operations",      EditorShell::PanelForm::Docked, {} },
			{ "save",            EditorShell::PanelForm::Docked, {} },
			{ "levels",          EditorShell::PanelForm::Docked, {} },
			// 独立窗口(用户指定):Widget Gallery 与 Input Map。
			{ "gallery",         EditorShell::PanelForm::Independent, { 120.0f, 120.0f, 520.0f, 400.0f } },
			{ "input",           EditorShell::PanelForm::Independent, { 660.0f, 120.0f, 440.0f, 340.0f } },
		};
	}

	EditorShell::EditorShell(EditorLayer& editor)
		: m_Editor(editor), m_LayoutPath(std::string(WLD_EDITOR_DIR) + "wui-layout.json")
	{
		// 面板列表与默认停靠布局都由形态声明生成:独立形态面板不进停靠树。
		// 注意:"windows"(Independent Windows 面板)与 "attach_slot" 仍未注册(等后续任务),
		// 因此也不在声明表里——布局存档里若残留它们的记录会被加载白名单丢弃。
		std::vector<Wui::PanelId> dockedPanels;
		for (const PanelSpec& spec : kPanelSpecs)
		{
			m_Panels.push_back(spec.Id);
			if (spec.Form == PanelForm::Docked)
				dockedPanels.push_back(spec.Id);
		}
		const Wui::DockLayout fallback = Wui::DockLayout::Default(dockedPanels);
		std::string error;
		if (!Wui::WuiLayoutStore::Load(m_LayoutPath, fallback, &m_Layout, &error))
			WLD_CORE_WARN("Failed to load WUI layout, using default: {0}", error);

		// 加载白名单(按声明纠正存档,不尝试"修复"矛盾记录):
		//  - 未声明的历史面板、声明为独立窗口的面板:从停靠树/浮动记录里直接丢弃;
		//  - 声明为停靠的面板:临时拖出不跨会话(回停靠树)。
		StripIndependentPanelsFromTree(m_Layout);
		RestoreDockedPanelsFromFloat(m_Layout);

		m_PanelRegistry.emplace("hierarchy", std::make_unique<HierarchyPanel>());
		m_PanelRegistry.emplace("properties", std::make_unique<PropertiesPanel>(*this));
		m_PanelRegistry.emplace("content_browser", std::make_unique<ContentBrowserPanel>(*this));
		m_PanelRegistry.emplace("view", std::make_unique<ViewportPanel>(*this));
		m_PanelRegistry.emplace("stats", std::make_unique<StatsPanel>());
		m_PanelRegistry.emplace("memory", std::make_unique<MemoryPanel>());
		m_PanelRegistry.emplace("operations", std::make_unique<OperationsPanel>());
		m_PanelRegistry.emplace("save", std::make_unique<SavePanel>());
		m_PanelRegistry.emplace("levels", std::make_unique<LevelPanel>());
		m_PanelRegistry.emplace("input", std::make_unique<InputMapPanel>());
		m_PanelRegistry.emplace("gallery", std::make_unique<WidgetGalleryPanel>());

		// 恢复"上次退出时开着"的独立窗口:存档里仍有浮动记录 = 上次开着(关掉的不会自动弹出)。
		// 按屏幕矩形分组重建:同一窗口的多个标签共享一个容器;AddFloatWindow 内部按面板去重。
		std::map<std::tuple<int, int, int, int>, std::vector<std::string>> floatGroups;
		for (const Wui::DockFloat& entry : m_Layout.Floating)
		{
			if (!IsIndependentPanel(entry.Panel))
				continue;
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

	// ---- 面板形态(单一事实源,见 EditorShell.h / T03 方案)----

	EditorShell::PanelForm EditorShell::FormOf(const std::string& panel) const
	{
		for (const PanelSpec& spec : kPanelSpecs)
			if (panel == spec.Id)
				return spec.Form;
		return PanelForm::Docked; // 未声明面板按停靠处理,加载白名单会把它们丢掉
	}

	bool EditorShell::IsDeclaredPanel(const std::string& panel) const
	{
		for (const PanelSpec& spec : kPanelSpecs)
			if (panel == spec.Id)
				return true;
		return false;
	}

	// 独立窗口只能以"独立窗口"存在:存档/撤销里的停靠树记录一律丢弃(不尝试修复)。
	// 同时清掉未声明的历史面板(如已下线的 "windows"),避免它们被保存路径写回停靠树。
	void EditorShell::StripIndependentPanelsFromTree(Wui::DockLayout& layout) const
	{
		std::vector<Wui::PanelId> docked;
		layout.AllPanels(&docked);
		for (const Wui::PanelId& panel : docked)
			if (!IsDeclaredPanel(panel) || IsIndependentPanel(panel))
				layout.RemoveTab(panel);

		const auto undeclared = [this](const Wui::DockFloat& entry) { return !IsDeclaredPanel(entry.Panel); };
		layout.Floating.erase(std::remove_if(layout.Floating.begin(), layout.Floating.end(), undeclared),
			layout.Floating.end());
		layout.FloatMemory.erase(std::remove_if(layout.FloatMemory.begin(), layout.FloatMemory.end(), undeclared),
			layout.FloatMemory.end());
	}

	// 停靠形态面板的"临时拖出"不跨会话:加载/保存时回停靠树(位置记忆仍保留在 FloatMemory)。
	void EditorShell::RestoreDockedPanelsFromFloat(Wui::DockLayout& layout) const
	{
		for (const PanelSpec& spec : kPanelSpecs)
		{
			if (spec.Form != PanelForm::Docked || !layout.IsFloating(spec.Id))
				continue;
			layout.Floating.erase(std::remove_if(layout.Floating.begin(), layout.Floating.end(),
				[&](const Wui::DockFloat& entry) { return entry.Panel == spec.Id; }), layout.Floating.end());
			if (layout.Contains(spec.Id))
				continue;
			const Wui::PanelId anchor = layout.FirstPanel();
			if (!anchor.empty())
			{
				layout.AddTab(spec.Id, anchor, Wui::DropZone::Center);
				continue;
			}
			// 停靠树为空(用户关掉了所有停靠面板):重建一个只含该面板的根 tab 组。
			Wui::DockLayout fresh;
			fresh.Floating = std::move(layout.Floating);
			fresh.FloatMemory = std::move(layout.FloatMemory);
			fresh.Root.Panels.push_back(spec.Id);
			fresh.Root.Active = 0;
			layout = std::move(fresh);
		}
	}

	Wui::DockLayout EditorShell::LayoutForSave() const
	{
		Wui::DockLayout out = m_Layout;
		StripIndependentPanelsFromTree(out);
		RestoreDockedPanelsFromFloat(out);
		return out;
	}

	Wui::WuiRect EditorShell::FloatRectFor(const std::string& panel) const
	{
		if (const auto remembered = m_LastFloatRects.find(panel); remembered != m_LastFloatRects.end())
			return remembered->second;
		for (const PanelSpec& spec : kPanelSpecs)
			if (panel == spec.Id && spec.Form == PanelForm::Independent)
				return spec.DefaultFloatRect;
		int windowX = 0, windowY = 0;
		if (Application::HasInstance())
			Application::Get().GetWindow().GetPosition(&windowX, &windowY);
		return { static_cast<float>(windowX) + 140.0f, static_cast<float>(windowY) + 100.0f, 480.0f, 340.0f };
	}

	Gameplay::SaveService* EditorShell::GetSaveService()
	{
		return m_Editor.GetSaveService();
	}

	bool EditorShell::IsReadOnlyMode() const
	{
		// Play/Simulate 期间面板只读查看:可选中/显示,但不改场景数据。
		return m_Editor.IsPlaying() || m_Editor.IsSimulating();
	}

	bool EditorShell::IsViewportCamera3D() const
	{
		return m_Editor.IsViewportCamera3D();
	}

	void EditorShell::ToggleViewportCamera3D()
	{
		m_Editor.ToggleViewportCamera3D();
	}

	Wui::GizmoCamera EditorShell::GetGizmoCamera() const
	{
		// gizmo 只依赖"投影 + 相机基向量 + 距离/FOV":2D 与 3D 视口各取一台相机。
		Wui::GizmoCamera camera;
		if (m_Editor.IsViewportCamera3D())
		{
			EditorCamera3D& source = m_Editor.GetEditorCamera3D();
			camera.ViewProjection = source.GetViewProjectionMatrix(/*vulkan=*/false);
			camera.Right = source.GetRight();
			camera.Up = source.GetUp();
			camera.Forward = source.GetForward();
			camera.Distance = source.GetDistance();
			camera.FovDegrees = source.GetFOV();
		}
		else
		{
			EditorCamera& source = m_Editor.GetEditorCamera();
			camera.ViewProjection = source.GetViewProjection();
			camera.Right = source.GetRightDirection();
			camera.Up = source.GetUpDirection();
			camera.Forward = source.GetForwardDirection();
			camera.Distance = source.GetDistance();
			camera.FovDegrees = source.GetFov();
		}
		return camera;
	}

	void EditorShell::ReleaseIndependentWindows()
	{
		// 退出时先显式销毁独立窗口:它们的 Vulkan 交换链/OS 窗口必须在
		// RHI 设备与主窗口销毁之前释放,否则会在关闭引擎时崩溃。
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (host)
				host->SetHidden(true); // 保持隐藏,直接进入销毁
		m_FloatHosts.clear();
		m_AttachedPanels.clear();
		m_ActiveWindowTag.clear();
	}

	void EditorShell::RecreateIndependentWindows()
	{
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (host && !host->IsHidden())
				host->RecreateWindow();
	}

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

	uint32_t EditorShell::TextureEpoch() const
	{
		return m_Editor.TextureEpoch();
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
		// 落盘前按形态声明规范化:独立窗口不进停靠树;停靠面板不持久化"临时拖出"。
		if (!Wui::WuiLayoutStore::Save(m_LayoutPath, LayoutForSave(), &error))
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
		// 已附加到主窗口(标签切换状态):菜单点击 = 关闭该窗口,
		// 隐藏其面板并把标签移出栏(此前只调 HideFloatPanel,会残留附加标签)。
		const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
		if (attached != m_AttachedPanels.end())
		{
			CloseFloatWindow(panel, true, &ctx);
			m_AttachedPanels.erase(attached);
			if (m_ActiveWindowTag == panel)
				m_ActiveWindowTag.clear();
			return;
		}
		// 独立窗口:开着 → 关闭(隐藏复用,不写回停靠树);关着 → 打开/复用同一窗口。
		if (IsIndependentPanel(panel))
		{
			if (m_Layout.IsFloating(panel))
			{
				HideFloatPanel(panel, &ctx);
				return;
			}
			OpenIndependentPanel(panel);
			RecordDockChange(ctx, "float", panel, before);
			return;
		}
		// 停靠形态面板:临时浮动着 → 回停靠位(D3);已停靠 → 隐藏;两者都不是 → 回到停靠树。
		if (m_Layout.IsFloating(panel))
		{
			if (DockPanelBackToTree(panel))
				RecordDockChange(ctx, "dock", panel, before);
			return;
		}
		if (m_Layout.Contains(panel))
		{
			if (m_Layout.RemoveTab(panel))
				RecordDockChange(ctx, "hide", panel, before);
			return;
		}
		{
			const std::string anchor = m_Layout.FirstPanel();
			if (anchor.empty())
			{
				Wui::DockLayout fresh;
				fresh.Root.Panels.push_back(panel);
				fresh.Root.Active = 0;
				m_Layout = std::move(fresh);
				RecordDockChange(ctx, "show", panel, before);
			}
			else if (m_Layout.AddTab(panel, anchor, Wui::DropZone::Center))
				RecordDockChange(ctx, "show", panel, before);
		}
	}

	// 停靠形态面板:从"临时拖出"回到停靠树(锚点 = 树里第一个面板;树为空则重建根 tab 组)。
	bool EditorShell::DockPanelBackToTree(const std::string& panel)
	{
		if (IsIndependentPanel(panel))
			return false;
		bool changed = false;
		if (m_Layout.IsFloating(panel))
		{
			m_Layout.Floating.erase(std::remove_if(m_Layout.Floating.begin(), m_Layout.Floating.end(),
				[&](const Wui::DockFloat& entry) { return entry.Panel == panel; }), m_Layout.Floating.end());
			changed = true;
		}
		if (m_Layout.Contains(panel))
			return changed;
		const Wui::PanelId anchor = m_Layout.FirstPanel();
		if (!anchor.empty())
			return m_Layout.AddTab(panel, anchor, Wui::DropZone::Center) || changed;
		const std::vector<Wui::DockFloat> floating = std::move(m_Layout.Floating);
		const std::vector<Wui::DockFloat> memory = std::move(m_Layout.FloatMemory);
		Wui::DockLayout fresh;
		fresh.Root.Panels.push_back(panel);
		fresh.Root.Active = 0;
		fresh.Floating = floating;
		fresh.FloatMemory = memory;
		m_Layout = std::move(fresh);
		return true;
	}

	// Window 菜单:打开独立形态面板 = 复用已隐藏的窗口(含它原有的其它标签)。
	void EditorShell::OpenIndependentPanel(const std::string& panel)
	{
		const Wui::WuiRect rect = FloatRectFor(panel);
		if (!m_Layout.IsFloating(panel))
			m_Layout.Floating.push_back({ panel, rect });
		m_LastFloatRects[panel] = rect;
		AddFloatWindow(panel, rect, "open");
	}

	void EditorShell::ResetLayout(Wui::WuiContext& ctx)
	{
		const std::string before = m_Layout.Serialize();
		// 重置也要按形态声明:独立窗口面板不进停靠树。
		std::vector<Wui::PanelId> dockedPanels;
		for (const PanelSpec& spec : kPanelSpecs)
			if (spec.Form == PanelForm::Docked)
				dockedPanels.push_back(spec.Id);
		m_Layout = Wui::DockLayout::Default(dockedPanels);
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
		// 独立窗口不参与四边停靠:它只能挂靠到顶部挂靠栏。
		const bool edgeDragIndependent = edgeDragActive
			&& IsIndependentPanel(edgePayload.substr(6));
		if (edgeDragActive && !edgeDragIndependent)
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
		// 已附加独立窗口时,主窗口作为"切换容器":当前标签是它,就显示它的内容。
		if (!m_ActiveWindowTag.empty())
		{
			const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), m_ActiveWindowTag);
			if (attached != m_AttachedPanels.end())
				RenderPanelContent(ctx, m_ActiveWindowTag, editorArea);
			else
				m_ActiveWindowTag.clear();
		}
		if (m_ActiveWindowTag.empty())
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
			Wui::DropZoneOverlay(ctx, zone, 0.30f, 3.0f);
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
				// 独立窗口只能挂靠到顶部挂靠栏:落到停靠区/面板一律忽略,窗口留在原处。
				if (IsIndependentPanel(panel))
				{
					m_DropTargetPanel.clear();
					m_DropZone = Wui::DropZone::Center;
					m_EdgeDockActive = false;
					m_EdgeDropZone = Wui::DropZone::Center;
					m_MovingFloat.clear();
				}
				else if (m_EdgeDockActive && (floating || m_Layout.Contains(panel)))
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
		// 拖拽结束且本帧未消费落点时,清除四边高亮:
		// 否则四边预览框会残留,表现成"启动/平时自动出现一个框"。
		if (!ctx.IsDragActive(nullptr))
		{
			m_EdgeDockActive = false;
			m_EdgeDropZone = Wui::DropZone::Center;
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
				&& m_TabDragPanel == m_DragPanel)
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
			m_TabDragPanel.clear();
		}
		else if (!m_DragPanel.empty())
		{
			// 拖拽结束:落点已在上方处理;未回收则保持浮动位置。
			m_DragPanel.clear();
			// 拖拽起点标记必须一起清掉:残留会让下一次"拖出"把已经收回停靠的面板
			// 立刻再次浮出(表现为关闭临时窗口后窗口又跳出来)。
			m_TabDragPanel.clear();
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
				// 分隔条走组件(Splitter):高亮/光标/命中统一。
				const Wui::WuiRect splitterArea = row
					? Wui::WuiRect { area.X + cursor - 2, area.Y, 4, area.H }
					: Wui::WuiRect { area.X, area.Y + cursor - 2, area.W, 4 };
				const Wui::SplitterResult split = Wui::Splitter(ctx, splitterArea, row, m_Theme, m_DragSplitNode == &node);
				if (split.Hovered && ctx.IsClicked(splitterArea))
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
		// 停靠标签栏统一走组件(WuiChrome::DockTabBar),主窗口与独立窗口外观/交互一致。
		std::vector<Wui::DockTab> tabs;
		tabs.reserve(node.Panels.size());
		for (size_t i = 0; i < node.Panels.size(); ++i)
			tabs.push_back({ Wui::HashId(("tab." + node.Panels[i]).c_str()), PanelTitle(node.Panels[i]), i == node.Active });
		const Wui::DockTabBarResult tabResult = Wui::DockTabBar(ctx, { area.X, area.Y, area.W, tabH }, tabs, m_Theme);

		if (tabResult.Clicked >= 0 && static_cast<size_t>(tabResult.Clicked) < node.Panels.size())
			m_Layout.Activate(node.Panels[tabResult.Clicked]);
		if (tabResult.Closed >= 0 && static_cast<size_t>(tabResult.Closed) < node.Panels.size())
		{
			const std::string before = m_Layout.Serialize();
			if (m_Layout.RemoveTab(node.Panels[tabResult.Closed]))
				RecordDockChange(ctx, "close", node.Panels[tabResult.Closed], before);
		}
		if (tabResult.DragStart >= 0 && static_cast<size_t>(tabResult.DragStart) < node.Panels.size())
		{
			const std::string& panel = node.Panels[tabResult.DragStart];
			ctx.BeginDrag(Wui::HashId(("tab." + panel).c_str()), "panel:" + panel);
			m_TabDragPanel = panel; // 记录本次拖拽的真实来源
		}

		const Wui::WuiRect content { area.X, area.Y + tabH, area.W, area.H - tabH };
		if (!node.Panels.empty())
			RenderPanelContent(ctx, node.Panels[node.Active], content);

		std::string dragPayload;
		// 独立窗口不能在停靠面板上落区(只能挂靠到顶部挂靠栏)。
		if (!m_EdgeDockActive && ctx.IsDragActive(&dragPayload) && dragPayload.rfind("panel:", 0) == 0
			&& !IsIndependentPanel(dragPayload.substr(6)) && ctx.IsHovered(area))
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
			// 落区预览走组件:与边缘停靠提示保持同一配色。
			Wui::PanelBackground(ctx, zone, { 0.30f, 0.50f, 0.90f, 0.28f }, 3.0f);
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
		Wui::PanelBackground(ctx, rect, m_Theme.PanelBg);
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
		Wui::BarSurface(ctx, bar, fill, m_Theme.Border);

		// 标准窗口控制(最小化/最大化/关闭)在最右侧。
		const Wui::WindowControl control = Wui::WindowControls(ctx,
			{ bar.X + bar.W - 102.0f, bar.Y, 102.0f, bar.H }, m_Theme,
			Application::HasInstance() && Application::Get().GetWindow().IsMaximized());
		if (control == Wui::WindowControl::Minimize)
		{
			if (Application::HasInstance())
				Application::Get().GetWindow().Minimize();
			return;
		}
		if (control == Wui::WindowControl::Maximize)
		{
			if (Application::HasInstance())
				Application::Get().GetWindow().MaximizeOrRestore();
			return;
		}
		if (control == Wui::WindowControl::Close)
		{
			m_Editor.CloseAction();
			return;
		}

		// 标签栏只列出"窗口":Main + 已附加到主窗口的独立窗口(切换关系)。
		float x = bar.X + 6.0f;
		{
			const Wui::WuiRect tab { x, bar.Y + 3.0f, 90.0f, bar.H - 6.0f };
			const bool active = m_ActiveWindowTag.empty();
			// 标签 chip 走组件:活动/悬停底色与关闭 x 的外观统一。
			if (Wui::AttachTag(ctx, tab, "Main", active, false, m_Theme).Clicked)
				m_ActiveWindowTag.clear();
			x += 96.0f;
		}
		x += 4.0f;

		// 已附加的独立窗口标签:点击切换;× 关闭该窗口;按住可拖出为独立窗口;
		// 多个附加窗口之间可左右拖动换位。
		std::string closeRequest;
		struct AttachTagHit { std::string Panel; Wui::WuiRect Rect; };
		std::vector<AttachTagHit> tagHits;
		for (const std::string& panel : m_AttachedPanels)
		{
			const bool active = m_ActiveWindowTag == panel;
			const Wui::WuiRect tab { x, bar.Y + 3.0f, 140.0f, bar.H - 6.0f };
			const Wui::AttachTagResult tag = Wui::AttachTag(ctx, tab, PanelTitle(panel), active, true, m_Theme);
			if (tag.CloseClicked)
				closeRequest = panel;
			// 按下(非关闭键)记录起点;移动超过阈值进入拖动。
			if (ctx.Input().MouseDown[0] && tag.Hovered && !tag.CloseHovered
				&& m_AttachTagDrag.empty())
			{
				m_AttachTagPress = panel;
				m_AttachTagPressPos = ctx.Input().MousePos;
			}
			if (m_AttachTagPress == panel && m_AttachTagDrag.empty() && ctx.Input().MouseDown[0]
				&& glm::length(ctx.Input().MousePos - m_AttachTagPressPos) > 3.0f)
				m_AttachTagDrag = panel;
			if (m_AttachTagDrag == panel)
			{
				Wui::HighlightOutline(ctx, tab, m_Theme.Accent, 2.0f, 2.0f);
				// 脱出提示:跟随光标的标签名 + 栏外时提示将变为独立窗口。
				const bool outside = !ctx.IsHovered(bar);
				ctx.PushOverlay();
				Label(ctx, { ctx.Input().MousePos.x + 14.0f, ctx.Input().MousePos.y + 14.0f },
					outside ? std::string(PanelTitle(panel)) + "  →  独立窗口" : std::string(PanelTitle(panel)),
					m_Theme.Text, 13.0f);
				ctx.PopOverlay();
			}
			tagHits.push_back({ panel, tab });
			x += 144.0f;
		}

		if (!closeRequest.empty())
		{
			// × = 关闭:隐藏该窗口的面板(可从 Window 菜单重新打开),并把标签移出栏。
			CloseFloatWindow(closeRequest, true, &ctx);
			m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), closeRequest),
				m_AttachedPanels.end());
			if (m_ActiveWindowTag == closeRequest)
				m_ActiveWindowTag.clear();
		}
		// 拖动结束:在栏内 → 换位;在栏外 → 拖出为独立窗口。
		if (m_AttachTagDrag.empty() && !m_AttachTagPress.empty() && ctx.Input().MouseReleased[0])
		{
			// 未拖动 = 单击:切换显示该窗口内容。
			m_ActiveWindowTag = m_AttachTagPress;
			m_AttachTagPress.clear();
		}
		if (!m_AttachTagDrag.empty() && ctx.Input().MouseReleased[0])
		{
			const std::string dragged = m_AttachTagDrag;
			if (ctx.IsHovered(bar))
			{
				size_t target = m_AttachedPanels.size();
				for (size_t i = 0; i < tagHits.size(); ++i)
					if (ctx.Input().MousePos.x < tagHits[i].Rect.X + tagHits[i].Rect.W * 0.5f)
					{
						target = i;
						break;
					}
				const auto current = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), dragged);
				if (current != m_AttachedPanels.end())
				{
					const size_t from = static_cast<size_t>(current - m_AttachedPanels.begin());
					m_AttachedPanels.erase(current);
					if (target > from && target > 0)
						--target;
					m_AttachedPanels.insert(m_AttachedPanels.begin()
						+ static_cast<std::ptrdiff_t>(std::min(target, m_AttachedPanels.size())), dragged);
				}
			}
			else if (FloatWindowHost* host = FindFloatHost(dragged))
			{
				// 拖出:恢复为独立窗口,窗口放到光标附近(屏幕坐标)。
				int mainX = 0, mainY = 0;
				if (Application::HasInstance())
					Application::Get().GetWindow().GetPosition(&mainX, &mainY);
				host->SetScreenPosition(static_cast<float>(mainX) + ctx.Input().MousePos.x - 60.0f,
					static_cast<float>(mainY) + ctx.Input().MousePos.y - 12.0f);
				host->SetHidden(false);
				m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), dragged),
					m_AttachedPanels.end());
				if (m_ActiveWindowTag == dragged)
					m_ActiveWindowTag.clear();
				ctx.RecordOp("float", "detach", dragged, "");
			}
			m_AttachTagPress.clear();
			m_AttachTagDrag.clear();
		}
		if (!ctx.Input().MouseDown[0] && m_AttachTagDrag.empty())
			m_AttachTagPress.clear();
		if (ctx.Input().MouseDown[0] && ctx.IsHovered(bar) && !ctx.IsHovered({ 0, 0, x, bar.H })
			&& !ctx.IsHovered({ bar.X + bar.W - 102.0f, bar.Y, 102.0f, bar.H }))
			Application::Get().GetWindow().BeginSystemDrag();
	}

	// 独立窗口:每个宿主 = 一个 OS 窗口(可含多个标签面板),绘制在停靠区之上。
	void EditorShell::RenderFloating(Wui::WuiContext& ctx)
	{
		if (m_AttachCooldownFrames > 0)
			--m_AttachCooldownFrames;
		// 每帧复位挂靠栏高亮,只在独立窗口真正悬停其上时点亮,避免残留。
		m_AttachSlotHighlight = false;

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
					candidate->SetTabDragActive(true);
					POINT cursor { 0, 0 };
					GetCursorPos(&cursor);
					const Wui::WuiRect rect = candidate->ScreenRect();
					m_CrossDragGrab = { static_cast<float>(cursor.x) - rect.X,
						static_cast<float>(cursor.y) - rect.Y };
					break;
				}
			}
		}
		// 拖拽期间窗口位置在渲染前更新,避免"渲染一帧后窗口才动"的迟滞感。
		if (m_CrossDragActive)
		{
			POINT cursor { 0, 0 };
			GetCursorPos(&cursor);
			if (FloatWindowHost* source = FindFloatHost(m_CrossDragPanel))
				source->SetScreenPosition(static_cast<float>(cursor.x) - m_CrossDragGrab.x,
					static_cast<float>(cursor.y) - m_CrossDragGrab.y);
		}
		for (size_t i = 0; i < m_FloatHosts.size(); )
		{
			FloatWindowHost& host = *m_FloatHosts[i];
			// 隐藏的宿主(复用中)不渲染。
			if (host.IsHidden())
			{
				++i;
				continue;
			}
			// 空宿主:隐藏保留(运行期销毁窗口在 Vulkan 下会崩),等待下次复用。
			if (host.Panels().empty())
			{
				host.SetHidden(true);
				++i;
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

			// 挂靠判定:拖动结束后光标落在主窗口顶栏(挂靠栏)上 → 挂靠;
			// 判定只在"拖动经过顶部栏"时点亮提示;真正的挂靠动作发生在松手时
			// (UpdateCrossWindowDrag)——悬停即挂靠会让拖动路径经过顶部栏时被截断。
			const std::string windowKey = host.Panels().front();
			m_LastFloatScreenRects[windowKey] = rect;
			POINT cursor { 0, 0 };
			GetCursorPos(&cursor);
			// 挂靠栏只接受独立窗口:停靠形态的临时浮动不参与挂靠(高亮也不点亮)。
			const bool cursorOverSlot = IsIndependentPanel(windowKey)
				&& m_AttachSlotScreenRect.W > 0.0f && m_AttachSlotScreenRect.H > 0.0f
				&& cursor.x >= static_cast<LONG>(m_AttachSlotScreenRect.X)
				&& cursor.x <= static_cast<LONG>(m_AttachSlotScreenRect.X + m_AttachSlotScreenRect.W)
				&& cursor.y >= static_cast<LONG>(m_AttachSlotScreenRect.Y)
				&& cursor.y <= static_cast<LONG>(m_AttachSlotScreenRect.Y + m_AttachSlotScreenRect.H);
			if (cursorOverSlot)
				m_AttachSlotHighlight = true;
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

		// 拖拽指示:按光标位置显示被拖动标签的名称(主窗口客户区坐标)。
		int mainX = 0, mainY = 0;
		if (Application::HasInstance())
			Application::Get().GetWindow().GetPosition(&mainX, &mainY);
		ctx.PushOverlay();
		Label(ctx, { pos.x - static_cast<float>(mainX) + 14.0f, pos.y - static_cast<float>(mainY) + 14.0f },
			PanelTitle(m_CrossDragPanel), m_Theme.Text, 13.0f);
		ctx.PopOverlay();

		// 独立窗口之间不是合法落点(T03:只能挂靠到顶部挂靠栏,不能互相附加标签)。
		m_CrossDragTargetKey.clear();
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			host->SetTabDropHighlight(false);

		// 挂靠栏(主窗口)也是有效落点,优先级高于其他独立窗口。
		const bool overAttachBar = IsIndependentPanel(m_CrossDragPanel)
			&& m_AttachSlotScreenRect.W > 0.0f
			&& pos.x >= m_AttachSlotScreenRect.X && pos.x <= m_AttachSlotScreenRect.X + m_AttachSlotScreenRect.W
			&& pos.y >= m_AttachSlotScreenRect.Y && pos.y <= m_AttachSlotScreenRect.Y + m_AttachSlotScreenRect.H;
		m_AttachSlotHighlight = overAttachBar;
		if (overAttachBar)
			m_CrossDragTargetKey.clear();

		// 松手(左键释放)才落点。
		if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
			return;

		const std::string panel = m_CrossDragPanel;
		FloatWindowHost* source = FindFloatHost(panel);
		if (source)
			source->SetTabDragActive(false);

		// 整窗挂靠到顶部挂靠栏(唯一合法落点)。
		if (overAttachBar)
		{
			AttachIndependentWindowToSlot(panel);
		}
		else if (source)
		{
			// 松手仍在本窗口标签栏上:标签重排(浏览器式拖动标签换位)。
			const Wui::WuiRect sourceRect = source->ScreenRect();
			const Wui::WuiRect sourceTabs { sourceRect.X, sourceRect.Y, sourceRect.W, 24.0f };
			if (pos.x >= sourceTabs.X && pos.x <= sourceTabs.X + sourceTabs.W
				&& pos.y >= sourceTabs.Y && pos.y <= sourceTabs.Y + sourceTabs.H)
			{
				const size_t count = source->Panels().size();
				const float slot = std::max(1.0f, (sourceRect.W - 8.0f) / static_cast<float>(count));
				const size_t index = static_cast<size_t>(std::max(0.0f,
					(pos.x - sourceRect.X - 4.0f) / std::min(150.0f, slot)));
				source->MovePanelTo(panel, index);
				ctx.RecordOp("float", "reorder", panel, std::to_string(index));
			}
			// 桌面空白:若源窗口还有其他标签,拆分为新独立窗口;单标签窗口只算移动。
			else if (source->Panels().size() > 1)
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
		// 去重(T03):同一面板最多一个窗口——已存在的窗口(可见或隐藏)直接复用,
		// 避免"同一面板被创建两次"这类布局乱象。
		if (FloatWindowHost* existing = FindFloatHost(panel))
		{
			existing->SetScreenPosition(screenRect.X, screenRect.Y);
			existing->ActivatePanel(panel);
			existing->SetHidden(false);
			WLD_CORE_INFO("[float] reused existing window for panel={0}", panel);
			return;
		}
		// 复用已隐藏的独立窗口:运行期销毁窗口在 Vulkan 下会崩,因此"关闭/挂靠"只隐藏。
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			if (!host->IsHidden())
				continue;
			host->SetScreenPosition(screenRect.X, screenRect.Y);
			host->AddPanel(panel, true);
			host->SetHidden(false);
			WLD_CORE_INFO("[float] reused hidden window for panel={0}", panel);
			return;
		}
		// 独立窗口 = 容器 + 标签栏;标题与内容由面板注册表提供,容器不感知具体面板类型。
		FloatWindowHost::Callbacks callbacks;
		callbacks.Theme = m_Theme;
		callbacks.Title = [this](const std::string& id) { return std::string(PanelTitle(id)); };
		callbacks.Content = [this](Wui::WuiContext& ctx, const Wui::WuiRect& rect, const std::string& id)
		{
			RenderPanelContent(ctx, id, rect);
		};
		callbacks.TabDragStart = [](const std::string& panel)
		{
			WLD_CORE_INFO("[float] tag drag started: {0}", panel);
		};
		callbacks.DockToMain = [this](const std::string& id) { AttachIndependentWindowToSlot(id); };
		callbacks.CloseWindow = [this](const std::string& id) { CloseFloatWindow(id, false, m_Ctx); };
		callbacks.CanAttach = [this](const std::string& id) { return IsIndependentPanel(id); };
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
		if (index >= m_FloatHosts.size() || m_FloatHosts[index]->IsHidden())
			return std::string();
		return m_FloatHosts[index]->Panel();
	}

	size_t EditorShell::IndependentWindowCount() const
	{
		size_t count = 0;
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			if (!host->IsHidden())
				++count;
		return count;
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
		for (const std::string& id : panels)
			host->RemovePanel(id);
		host->SetHidden(true); // 复用:隐藏而非销毁(运行期销毁窗口会崩)

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
		// 只有声明为"独立窗口"的面板能挂靠到顶部挂靠栏;停靠形态的临时浮动不允许挂靠。
		if (!IsIndependentPanel(panel))
		{
			WLD_CORE_INFO("[float] panel '{0}' is a docked panel; attach ignored", panel);
			return;
		}
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		for (const std::string& id : panels)
			m_LastFloatRects[id] = rect;
		// 附加 = 与主窗口建立"标签切换"关系:窗口与其面板保持不变,只隐藏 OS 窗口;
		// 主窗口顶栏出现该标签,点击即在 主界面 / 该窗口内容 之间切换。
		host->SetHidden(true);
		if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) == m_AttachedPanels.end())
			m_AttachedPanels.push_back(panel);
		m_ActiveWindowTag = panel;
		WLD_CORE_INFO("Independent window attached as switch tab: {0} (still in dock tree: {1})",
			panel, m_Layout.Contains(panel) ? "yes" : "no");

		// 不再把面板并入停靠树(那是旧"挂靠"语义);附加只建立标签切换关系。
		m_AttachSlotHighlight = false;
		m_DragPanel.clear();
		m_MovingFloat.clear();
		m_TabDragPanel.clear();
		m_LastDragPos = { 0, 0 };
		m_AttachCooldownFrames = 45;
		WLD_CORE_INFO("Independent window attached to slot: {0} ({1} panels)", panel, panels.size());
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
				host->SetHidden(true); // 复用:隐藏而非销毁
		}
		m_LastFloatScreenRects.erase(panel);
		// 独立形态:隐藏即从浮动记录移除(Window 菜单可重开);
		// 停靠形态:临时拖出的标签关闭 = 回停靠位(D3)。
		const bool changed = IsIndependentPanel(panel)
			? m_Layout.CloseFloating(panel)
			: DockPanelBackToTree(panel);
		if (changed && ctx)
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
			// 停靠形态的"临时浮动"关闭后应回停靠位(独立形态保持隐藏)。
			if (!IsIndependentPanel(panel))
				DockPanelBackToTree(panel);
			return;
		}
		const std::vector<std::string> panels = host->Panels();
		const Wui::WuiRect rect = host->ScreenRect();
		for (const std::string& id : panels)
			host->RemovePanel(id);
		host->SetHidden(true); // 复用:隐藏而非销毁
		bool changed = false;
		for (const std::string& id : panels)
		{
			if (Wui::DockFloat* entry = m_Layout.FindFloat(id))
				entry->Rect = rect;
			// 独立形态:关闭 = 隐藏复用(移除浮动记录,便于 Window 菜单重开)。
			// 停靠形态:临时拖出不跨会话,直接回停靠树(D3)。
			if (IsIndependentPanel(id))
			{
				m_LastFloatRects[id] = rect;
				changed = m_Layout.CloseFloating(id) || changed;
			}
			else
			{
				changed = DockPanelBackToTree(id) || changed;
			}
		}
		if (changed && recordChange && ctx)
			RecordDockChange(*ctx, "hide", panel, before);
	}

	void EditorShell::DrawMenuBar(Wui::WuiContext& ctx)
	{
		const glm::vec2 viewport = ctx.ViewportSize();

		// 菜单栏属于"当前窗口":切到已附加的独立窗口内容时,显示它自己的菜单栏
		// (Widget 目前为空),而不是主窗口的 File/Window 菜单。
		if (!m_ActiveWindowTag.empty())
		{
			Wui::PanelBackground(ctx, { 0, 26, viewport.x, 26 }, m_Theme.PanelHeader);
			return;
		}

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
		Wui::PanelBackground(ctx, { 0, 26, viewport.x, 26 }, m_Theme.PanelHeader);
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

