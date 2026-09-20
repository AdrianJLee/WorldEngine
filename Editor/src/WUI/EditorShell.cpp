#include "wldpch.h"
#include "EditorShell.h"

#include <cstdio>
#include <fstream>
#include "../EditorLayer.h"

#include "World/Core/Asset/ProjectManifest.h"
#include "World/Core/KeyCodes.h"
#include "World/Renderer/Renderer.h"
#include "World/WUI/WuiLayoutStore.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiAccessibility.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/WUI/Widgets/WuiModal.h"
#include "Panels/MaterialEditorPanel.h"
#include "Panels/ModelPreviewPanel.h"
#include "Panels/SettingsPanel.h"
#include "Panels/PreferencesPanel.h"
#include "World/WUI/WuiLocalization.h"

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

		// P3-1②:挂靠标签(AttachTag)的无障碍登记 —— 稳定 id `shell.attach.<panel>`,
		// value 标明面板当前是"附加态(attached)"还是"浮动态(floating)",脚本据此断言
		// ui.detach / ui.attach 的结果,与 state.dump 的 "attach" 段同源(PanelStateLabel)。
		// Window 显式写 "main":登记发生在挂靠栏绘制时,但这一枚标签属于主窗口
		// (ui.invoke 的注入点击必须送到主窗口,而不是本轮最后一个渲染过的独立窗口)。
		void RegisterAttachNode(const std::string& panel, const Wui::WuiRect& rect, const char* state,
			std::string label, bool interactive)
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId(("shell.attach." + panel).c_str());
			node.Window = "main";
			node.Panel = panel;
			node.Kind = "attach-tag";
			node.Label = std::move(label);
			node.Value = state;
			node.Rect = rect;
			node.Enabled = true;
			node.Interactive = interactive;
			Wui::WuiAccessibility::Get().Register(node);
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
			// 项目设置(用户 2026-09-20:应为独立窗口):声明为独立窗口形态 —— 默认打开仍走
			// TogglePanel → OpenPanelAttached(附加到主窗口的标签切换),可拖出为独立 OS 窗口。
			// 布局存档里的旧停靠记录由 StripIndependentPanelsFromTree 丢弃(不会再停靠回树)。
			{ "settings",        EditorShell::PanelForm::Independent, { 220.0f, 140.0f, 560.0f, 460.0f } },
			// P4-UX1:编辑器偏好(用户级,自动保存):语言/主题/缩放/术语对照。
			// P4-UX2:左侧分类 + 右侧内容,默认尺寸更大;位置由 FloatRectFor 居中。
			{ "prefs",           EditorShell::PanelForm::Independent, { 0.0f, 0.0f, 900.0f, 560.0f } },
			{ "memory",          EditorShell::PanelForm::Docked, {} },
			{ "operations",      EditorShell::PanelForm::Docked, {} },
			{ "save",            EditorShell::PanelForm::Docked, {} },
			{ "levels",          EditorShell::PanelForm::Docked, {} },
			// 独立窗口(用户指定):Widget Gallery、Input Map、Scripts(与 2026-09-20 起的
			// Project Settings)。
			{ "gallery",         EditorShell::PanelForm::Independent, { 120.0f, 120.0f, 520.0f, 400.0f } },
			{ "input",           EditorShell::PanelForm::Independent, { 660.0f, 120.0f, 440.0f, 340.0f } },
			// W8:Scripts 面板(独立窗口,诊断行/按钮在默认客户区内,便于无鼠标自动化)。
			{ "scripts",         EditorShell::PanelForm::Independent, { 120.0f, 160.0f, 760.0f, 470.0f } },
			// 注:D3 材质编辑器是**动态面板**(每个材质一个 "material:<path>" 实例),
			// 不在这张静态声明表里,由 IsMaterialPanel/EditorShell::OpenMaterialEditor 处理。
		};

		// 动态材质面板 id 前缀:每个材质一个面板/独立窗口(用户 2026-09-16 要求)。
		constexpr const char* kMaterialPanelPrefix = "material:";
		// P1b D5:模型预览面板(每个 .wmodel 一个,打开=只读预览,不改场景)。
		constexpr const char* kModelPanelPrefix = "model:";
		// W9-2:动态脚本编辑器面板 id 前缀:每个脚本一个 "script:<逻辑路径>" 面板。
		constexpr const char* kScriptPanelPrefix = "script:";
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
		// P4-UX1:主题来源单一化 —— 默认暗色,`WLD_UI_THEME=light|system` 可覆盖。
		m_Theme = Wui::CurrentTheme();
		m_ThemeGeneration = Wui::ThemeGeneration();
		m_PanelRegistry.emplace("properties", std::make_unique<PropertiesPanel>(*this));
		m_PanelRegistry.emplace("content_browser", std::make_unique<ContentBrowserPanel>(*this));
		m_PanelRegistry.emplace("view", std::make_unique<ViewportPanel>(*this));
		m_PanelRegistry.emplace("stats", std::make_unique<StatsPanel>());
		// D8a2:项目渲染设置(引擎用户可配置)。用户 2026-09-20 指定为独立窗口形态:
		// 默认打开 = 附加到主窗口(见 PanelSpec),可拖出为独立 OS 窗口 / ui.detach。
		m_PanelRegistry.emplace("settings", std::make_unique<SettingsPanel>());
		m_PanelRegistry.emplace("prefs", std::make_unique<PreferencesPanel>());
		m_PanelRegistry.emplace("memory", std::make_unique<MemoryPanel>());
		m_PanelRegistry.emplace("operations", std::make_unique<OperationsPanel>());
		m_PanelRegistry.emplace("save", std::make_unique<SavePanel>());
		m_PanelRegistry.emplace("levels", std::make_unique<LevelPanel>());
		m_PanelRegistry.emplace("input", std::make_unique<InputMapPanel>());
		m_PanelRegistry.emplace("gallery", std::make_unique<WidgetGalleryPanel>());
		m_PanelRegistry.emplace("material", std::make_unique<MaterialEditorPanel>());
		m_PanelRegistry.emplace("scripts", std::make_unique<ScriptsPanel>());

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
			// 动态材质面板按上面白名单被判定为"已声明独立面板",这里补建实例(路径编码在 id 里)。
			for (const std::string& panel : panels)
			{
				EnsureMaterialPanelFromId(panel);
				EnsureScriptPanelFromId(panel);
				EnsureModelPanelFromId(panel);
			}
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
		// 动态材质面板:每个材质一个独立窗口(不在静态声明表里)。
		if (panel.compare(0, std::strlen(kMaterialPanelPrefix), kMaterialPanelPrefix) == 0)
			return PanelForm::Independent;
		if (panel.compare(0, std::strlen(kModelPanelPrefix), kModelPanelPrefix) == 0)
			return PanelForm::Independent;
		// W9-2:动态脚本编辑器面板同上(打开后默认附加到主窗口,仍属"独立窗口"形态:
		// 可拖出为 OS 窗口 / 再挂靠回主窗口)。
		if (panel.compare(0, std::strlen(kScriptPanelPrefix), kScriptPanelPrefix) == 0)
			return PanelForm::Independent;
		for (const PanelSpec& spec : kPanelSpecs)
			if (panel == spec.Id)
				return spec.Form;
		return PanelForm::Docked; // 未声明面板按停靠处理,加载白名单会把它们丢掉
	}

	bool EditorShell::IsDeclaredPanel(const std::string& panel) const
	{
		if (panel.compare(0, std::strlen(kMaterialPanelPrefix), kMaterialPanelPrefix) == 0)
			return true;
		if (panel.compare(0, std::strlen(kModelPanelPrefix), kModelPanelPrefix) == 0)
			return true;
		if (panel.compare(0, std::strlen(kScriptPanelPrefix), kScriptPanelPrefix) == 0)
			return true;
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
		Wui::WuiRect rect {};
		for (const PanelSpec& spec : kPanelSpecs)
			if (panel == spec.Id && spec.Form == PanelForm::Independent)
			{
				rect = spec.DefaultFloatRect;
				break;
			}
		int windowX = 0, windowY = 0;
		float clientW = 0.0f, clientH = 0.0f;
		if (Application::HasInstance())
		{
			Application::Get().GetWindow().GetPosition(&windowX, &windowY);
			clientW = static_cast<float>(Application::Get().GetWindow().GetWidth());
			clientH = static_cast<float>(Application::Get().GetWindow().GetHeight());
		}
		if (rect.W <= 0.0f || rect.H <= 0.0f)
			rect = { 0.0f, 0.0f, 480.0f, 340.0f };
		// P4-UX2c:默认矩形是**设计单位**,窗口是物理像素:按内容缩放放大,
		// 否则 1.3 缩放下面板只有 77% 的设计空间(实测偏好面板右侧说明被裁掉)。
		const float uiScale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		rect.W *= uiScale;
		rect.H *= uiScale;
		// P4-UX2(用户反馈"全屏时位置太靠左"):没有位置记忆的面板默认落在**主窗口客户区正中**;
		// 用户拖过之后走 m_LastFloatRects / FloatMemory,不会再被居中覆盖。
		rect.X = static_cast<float>(windowX) + (clientW > rect.W ? (clientW - rect.W) * 0.5f : 40.0f);
		rect.Y = static_cast<float>(windowY) + (clientH > rect.H ? (clientH - rect.H) * 0.5f : 40.0f);
		return rect;
	}

	Gameplay::SaveService* EditorShell::GetSaveService()
	{
		return m_Editor.GetSaveService();
	}

	// W8:Scripts 面板的三个宿主能力 —— 只做转发,策略全部留在 EditorLayer(路径解析/唯一重载入口)。
	bool EditorShell::ScriptsReloadInstance(entt::entity handle, std::string* message)
	{
		return m_Editor.ScriptsReloadInstance(handle, message);
	}

	bool EditorShell::ScriptsOpenExternal(const std::string& logicalPath, std::string* message)
	{
		return m_Editor.ScriptsOpenExternal(logicalPath, message);
	}

	bool EditorShell::ScriptsCreateFromTemplate(std::string& outLogicalPath, std::string* message)
	{
		return m_Editor.ScriptsCreateFromTemplate(outLogicalPath, message);
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
		// Play(且未暂停):视口渲染走场景的**主相机实体**(见 EditorLayer::OnUpdate 的 Play 分支;
		// 暂停时该分支会切回编辑器相机 —— 覆盖层必须跟渲染保持同一条件,否则暂停后又错位),
		// 覆盖层(选中框/gizmo)必须跟着同一台相机,否则位置整体错位
		// (用户 2026-09-16:"Play 后点物体,选中框位置不对")。
		if (m_Editor.IsPlaying() && !m_Editor.IsPaused())
		{
			if (Ref<Scene> scene = m_Editor.GetActiveScene())
			{
				Entity cameraEntity = scene->GetPrimaryCameraEntity();
				if (cameraEntity.IsValid() && cameraEntity.HasComponent<CameraComponent>() &&
					cameraEntity.HasComponent<TransformComponent>())
				{
					glm::mat4 world = cameraEntity.GetComponent<TransformComponent>().Transform;
					if (cameraEntity.HasComponent<WorldTransformComponent>())
						world = cameraEntity.GetComponent<WorldTransformComponent>().Matrix;
					const Camera& playCamera = cameraEntity.GetComponent<CameraComponent>().Camera;
					camera.ViewProjection = playCamera.GetProjectionMatrix() * glm::inverse(world);
					camera.Position = glm::vec3(world[3]);
					// 与 EditorCamera3D 同约定:Forward = 看向场景内的方向(相机局部 -Z)。
					camera.Forward = -glm::normalize(glm::vec3(world[2]));
					camera.Right = glm::normalize(glm::vec3(world[0]));
					camera.Up = glm::normalize(glm::vec3(world[1]));
					// 透视投影反推竖直 FOV(gizmo 的"屏幕恒定尺寸"用);距离取相机到选中实体。
					const glm::mat4& projection = playCamera.GetProjectionMatrix();
					camera.FovDegrees = std::abs(projection[1][1]) > 1e-6f
						? glm::degrees(2.0f * std::atan(1.0f / std::abs(projection[1][1]))) : 45.0f;
					Entity selected = m_Editor.GetSelectedEntity();
					if (selected.IsValid() && selected.GetScene() == scene.get() &&
						selected.HasComponent<TransformComponent>())
					{
						glm::mat4 selectedWorld = selected.GetComponent<TransformComponent>().Transform;
						if (selected.HasComponent<WorldTransformComponent>())
							selectedWorld = selected.GetComponent<WorldTransformComponent>().Matrix;
						camera.Distance = std::max(0.1f, glm::length(camera.Position - glm::vec3(selectedWorld[3])));
					}
					else
						camera.Distance = 10.0f;
					return camera;
				}
			}
		}
		if (m_Editor.IsViewportCamera3D())
		{
			EditorCamera3D& source = m_Editor.GetEditorCamera3D();
			camera.ViewProjection = source.GetViewProjectionMatrix(/*vulkan=*/false);
			camera.Right = source.GetRight();
			camera.Up = source.GetUp();
			camera.Forward = source.GetForward();
			camera.Position = source.GetPosition();
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
			camera.Position = source.GetPosition();
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

	bool EditorShell::DebugClickHierarchyRow(size_t index)
	{
		const auto it = m_PanelRegistry.find("hierarchy");
		if (it == m_PanelRegistry.end() || !it->second)
			return false;
		auto* panel = dynamic_cast<HierarchyPanel*>(it->second.get());
		return panel && panel->DebugInvokeRowClick(index);
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

	uint64_t EditorShell::GetCameraPreviewTextureId() const
	{
		return m_Editor.GetCameraPreviewTextureId();
	}

	bool EditorShell::IsCameraPreviewEnabled() const
	{
		return m_Editor.IsCameraPreviewEnabled();
	}

	void EditorShell::ToggleCameraPreview()
	{
		m_Editor.ToggleCameraPreview();
	}

	std::string EditorShell::CameraPreviewLabel() const
	{
		return m_Editor.CameraPreviewLabel();
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

	std::string EditorShell::PanelTitle(const std::string& id) const
	{
		// 静态面板走本地化表;动态面板(材质/模型/脚本)用注册表里的动态标题。
		static const std::pair<const char*, const char*>* kTitles = nullptr;
		static const std::map<std::string, std::pair<const char*, const char*>> titles {
			{ "hierarchy",       { "panel.hierarchy", "Hierarchy" } },
			{ "properties",      { "panel.properties", "Properties" } },
			{ "content_browser", { "panel.content_browser", "Content Browser" } },
			{ "view",            { "panel.view", "View" } },
			{ "stats",           { "panel.stats", "Stats" } },
			{ "settings",        { "panel.settings", "Project Settings" } },
			{ "prefs",           { "panel.prefs", "Editor Preferences" } },
			{ "memory",          { "panel.memory", "Memory" } },
			{ "operations",      { "panel.operations", "Operations" } },
			{ "save",            { "panel.save", "Save" } },
			{ "levels",          { "panel.levels", "Levels" } },
			{ "gallery",         { "panel.gallery", "Widget Gallery" } },
			{ "input",           { "panel.input", "Input Map" } },
			{ "scripts",         { "panel.scripts", "Scripts" } },
		};
		(void)kTitles;
		if (const auto found = titles.find(id); found != titles.end())
			return Wui::Tr(found->second.first, found->second.second);
		const auto it = m_PanelRegistry.find(id);
		return it != m_PanelRegistry.end() ? it->second->Title() : id;
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

	// ---- P3-1①:面板拖拽状态归零(唯一出口)----
	// "这次拖拽是谁":拖起手标签 / 跟随窗口 / 落点位置。松手那一帧也要清 ——
	// 残留的 m_TabDragPanel/m_DragPanel 会让面板一回到停靠树就被再次浮出(P3-1① 的 bug)。
	void EditorShell::ClearPanelDragIdentity()
	{
		m_DragPanel.clear();
		m_LastDragPos = { 0.0f, 0.0f };
		m_TabDragPanel.clear();
		m_MovingFloat.clear();
		m_FloatGrabOffset = { 0.0f, 0.0f };
	}

	// 完整归零:身份成员 + 落点成员 + 挂靠标签拖拽。
	// 落点成员(m_DropTargetPanel/m_EdgeDockActive/…)是"最后一帧武装的目标",WuiContext 要到
	// **下一帧**才交付 AcceptDrop —— 所以在"释放沿"那一帧只能清身份成员(见状态机里结束分支的
	// 说明),只有落点已消费或显式取消(Esc)时才走这个完整版本。
	void EditorShell::ClearPanelDragState()
	{
		ClearPanelDragIdentity();
		m_DropTargetPanel.clear();
		m_DropZone = Wui::DropZone::Center;
		m_EdgeDockActive = false;
		m_EdgeDropZone = Wui::DropZone::Center;
		m_LastDragTarget.clear();
		m_LastDragZone = Wui::DropZone::Center;
		m_DropPreviewActive = false;
		m_AttachTagPress.clear();
		m_AttachTagDrag.clear();
	}

	// 拖拽确定已经结束(drop 已消费 / 松手未落点 / Esc 取消 / 左键已抬起)时的收口。
	void EditorShell::EndPanelDrag(Wui::WuiContext& ctx)
	{
		ClearPanelDragState();
		// ctx 里的 dragging/pending/payload/drop 标记一起清:否则下一条命令/下一帧仍会看到
		// "payload 是 panel:xxx"的残留拖拽(正是"面板刚回到停靠树就再次浮出"的原料)。
		ctx.EndDrag();
	}

	// 面板当前形态标签(单一事实源):AiDetachPanel/AiAttachPanel 的幂等判定、state.dump 的
	// "attach" 段、AttachTag 无障碍节点的 value 都用它,避免三处各写一套判定。
	const char* EditorShell::PanelStateLabel(const std::string& panel) const
	{
		if (IsIndependentPanel(panel))
		{
			// 独立窗口形态只有三种状态:顶栏标签(attached)/ 真 OS 窗口(floating)/ 关闭。
			if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end())
				return "attached";
			for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			{
				if (!host->Contains(panel))
					continue;
				if (!host->IsHidden())
					return "floating";
				// 隐藏宿主:窗口里只要有一个标签在附加列表里,本面板就属于那枚顶栏标签
				// (窗口被拆成多个标签页时,state.dump / 无障碍节点都要说真话)。
				for (const std::string& id : host->Panels())
					if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), id) != m_AttachedPanels.end())
						return "attached";
				break;
			}
			return "hidden";
		}
		// 停靠形态面板:docked(在停靠树里)/ floating(主窗口内临时浮动)/ hidden。
		if (m_Layout.Contains(panel))
			return "docked";
		if (m_Layout.IsFloating(panel))
			return "floating";
		return "hidden";
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
			// D10:默认打开 = **附加到主窗口**(用户 2026-09-19)。Window 菜单与 AI `ui.open`
			// 都走这条,所以这一处覆盖所有"声明为独立形态"的面板(gallery/input/scripts)。
			OpenPanelAttached(panel);
			RecordDockChange(ctx, "attach", panel, before);
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
			// 优先回到该面板**上次所在的标签组**(RenderTabs 每帧刷新锚点表);
			// 该组已不存在时再退回 FirstPanel,树为空则重建根组。
			std::string anchor;
			if (const auto remembered = m_LastDockAnchors.find(panel); remembered != m_LastDockAnchors.end() &&
				m_Layout.Contains(remembered->second))
				anchor = remembered->second;
			else
				anchor = m_Layout.FirstPanel();
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

	void EditorShell::OpenPanelAttached(const std::string& panel)
	{
		// D10(用户 2026-09-19):**所有独立窗口默认附加到主窗口**。
		//  - 已经附加 → 只激活对应标签;
		//  - 用户此前把它拖成了独立窗口(布局里有浮动记录)→ 尊重现状,只前置焦点;
		//  - 否则先按独立窗口建出(复用已隐藏的窗口),随后立即挂靠到主窗口。
		// 显式分离(拖出/菜单)仍然可用 —— 这个函数只改"默认打开"的落点。
		if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end())
		{
			m_ActiveWindowTag = panel;
			return;
		}
		if (m_Layout.IsFloating(panel))
		{
			FocusIndependentWindow(panel);
			return;
		}
		if (!FindFloatHost(panel))
			OpenIndependentPanel(panel);
		AttachIndependentWindowToSlot(panel);
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
		// P4-UX1:主题模式(暗/浅/跟随系统)变化时刷新本地副本;独立窗口在下次创建/附加时
		// 取到新主题(它们的 callbacks.Theme 是创建期快照)。
		if (m_ThemeGeneration != Wui::ThemeGeneration())
		{
			m_Theme = Wui::CurrentTheme();
			m_ThemeGeneration = Wui::ThemeGeneration();
		}
		// W9-2 修复:上一帧被推迟的脚本编辑器打开请求,在帧边界统一执行(安全点)。
		if (!m_PendingScriptOpen.empty())
		{
			std::vector<std::string> pending;
			pending.swap(m_PendingScriptOpen);
			for (const std::string& path : pending)
				OpenScriptEditorNow(path);
		}
		// AI 无障碍树:主窗口这一帧的节点从这里开始重新登记(见 WuiAccessibility)。
		Wui::WuiAccessibility::Get().BeginFrame("main", ctx.ViewportSize());
		// W9 review:文本焦点登记用宿主显式身份,不依赖无障碍开关。
		ctx.SetWindowKey("main");
		m_ViewportRect = {};
		ctx.ClearDropTarget();
		m_DropPreviewActive = false;
		// W9-2:Ctrl+Z/Y 与脚本编辑器的 buffer 撤销/重做冲突 —— 文本焦点活跃(上一帧快照)
		// 或焦点面板是脚本编辑器时,场景撤销/重做让位(WuiCodeEditor 自己消费这组键)。
		EditorPanel* const focusPanelAtFrameStart = FocusedPanel();
		const bool scriptEditorFocused = focusPanelAtFrameStart
			&& std::strncmp(focusPanelAtFrameStart->Id(), kScriptPanelPrefix, std::strlen(kScriptPanelPrefix)) == 0;
		const bool sceneUndoAllowed = !m_TextFocusLatched && !scriptEditorFocused;
		const bool undoKey = sceneUndoAllowed && ctx.Input().Ctrl && !ctx.Input().Shift
			&& ctx.IsKeyPressed(KeyCodes::Z);
		const bool redoKey = sceneUndoAllowed && ctx.Input().Ctrl
			&& (ctx.IsKeyPressed(KeyCodes::Y) || (ctx.Input().Shift && ctx.IsKeyPressed(KeyCodes::Z)));
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
		// D10-11(用户 2026-09-19"这种窗口也该抽象出来"):导入位置是**窗口级模态** ——
		// 开帧用 WuiModal 的输入封锁把整个客户区登记成遮挡区,后面画的所有面板照常显示但
		// 收不到命中(点击/悬停/拖放都走 HitTest);画模态本体之前 EndModalInputBlock 解开。
		// D10-15:shell 的四个模态(未保存/错误/打包/项目设置)同样是真模态,同一口径挡输入。
		// 项目设置用"请求标志 || 当前模态 id"判定:菜单点击发生在同一帧的 DrawMenuBar,
		// 请求标志当帧为真;之后由 ctx 里留着的模态 id 负责(DrawModals 才消费请求标志)。
		const Wui::WuiId projectSettingsModalId = Wui::HashId("modal.projectsettings");
		const bool shellModalOpen = m_ImportModalOpen || m_Editor.ShowUnsavedModal()
			|| m_Editor.ShowErrorModal() || m_Editor.ShowCookingProgress() || m_ShowProjectSettings
			|| ctx.Modal() == projectSettingsModalId;
		if (shellModalOpen)
			Wui::BeginModalInputBlock(ctx);
		// 编辑器级四边停靠区:拖拽面板进入窗口边缘条带时,生成横跨整个编辑器的
		// 停靠区(而不是只切分鼠标所在的面板组)。需在渲染面板前判定,以便
		// RenderTabs 跳过面板内的落区逻辑。
		const float editorTop = 26.0f + m_AttachBarHeight;
		// P4-UX6:底部留出状态栏(场景/选择/后端/帧率)——面板不再压到它上面。
		constexpr float statusBarHeight = 22.0f;
		const Wui::WuiRect statusBar { 0, viewport.y - statusBarHeight, viewport.x, statusBarHeight };
		const Wui::WuiRect editorArea { 0, editorTop, viewport.x,
			viewport.y - editorTop - statusBarHeight };
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
				// 四边停靠判定带(用户 2026-09-16:110px 太大,压到面板中部)。
				constexpr float edgeBand = 64.0f;
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
		// 窗口内浮动面板(停靠形态)画在停靠区之上,命中也要优先:先登记它们的矩形为
		// 遮挡区,下面的面板就不会同时响应;正在拖动的那一个除外(它跟随光标,且必须
		// 让下方的停靠落点能被命中)。
		for (const Wui::DockFloat& entry : m_Layout.Floating)
			if (!IsIndependentPanel(entry.Panel) && entry.Panel != m_MovingFloat)
				ctx.PushHoverBlocker(entry.Rect);
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
			// 预览延迟到 RenderFloating 里画(浮动面板之上),否则会被拖动中的面板盖住。
			m_DropPreviewRect = zone;
			m_DropPreviewActive = true;
		}
		// 下层绘制结束:解除遮挡,浮动面板/菜单/弹窗仍按真实光标命中。
		if (shellModalOpen)
			Wui::EndModalInputBlock(ctx);   // 与上面的 BeginModalInputBlock 成对
		else
			ctx.ClearHoverBlockers();       // 无模态:清掉窗口内浮动面板的遮挡区(原行为)
		// 模态之下还有浮动面板与菜单栏要画,继续挡住它们的命中(它们照常显示,
		// 但点不到);到画模态前再解除(见 DrawModals 前后)。
		if (shellModalOpen)
			Wui::BeginModalInputBlock(ctx);

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
			// P3-1①:落点是拖拽的**正常出口**,这里一次性把外壳与 WuiContext 的拖拽
			// 状态全部归零(payload 刚被 AcceptDrop 消费,清掉不会丢落点)。
			EndPanelDrag(ctx);
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
		// P3-1①:状态机的四个出口(落点消费 / 松手未落点 / Esc 取消 / 左键已抬)都必须
		// 让拖拽状态归零 —— 否则残留的 m_TabDragPanel/m_DragPanel 会在面板**回到停靠树**
		// 的那一帧再次执行拖出分支(实测表现:浮窗刚被 ✕ 关掉又跳回来,第一次点 ✕ 被吃掉)。
		// 左键**物理**状态是不依赖事件送达的判据:释放落在别的窗口/窗口外时,本窗口的
		// 释放事件可能永远不到(与 WuiInputCollector::SyncButtonsWithSystem、
		// FloatWindowHost::Render 同一口径)。
		const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		{
			// Esc = 取消(左键可能还按着):一次把 WuiContext 与外壳状态都归零,
			// 之后即使按键还按着,也不会因为"按住即报"的 DragStart 被重新武装(见 RenderTabs)。
			std::string escPayload;
			const bool panelDragInFlight = !m_DragPanel.empty()
				|| (ctx.IsDragActive(&escPayload) && escPayload.rfind("panel:", 0) == 0);
			if (panelDragInFlight && ctx.WasKeyPressed(KeyCodes::Escape))
				EndPanelDrag(ctx);
		}
		std::string activePayload;
		if (leftButtonDown && ctx.IsDragActive(&activePayload) && activePayload.rfind("panel:", 0) == 0)
		{
			m_DragPanel = activePayload.substr(6);
			m_LastDragPos = ctx.Input().MousePos;
			// 只有"本次拖拽确实起手于该面板的标签页"时才允许拖出为独立窗口;
			// 否则残留的拖拽状态会在挂靠后立刻把面板再次浮出(表现为多出一个窗口)。
			if (!dropConsumed && m_AttachCooldownFrames <= 0 && m_Layout.Contains(m_DragPanel)
				&& m_TabDragPanel == m_DragPanel)
			{
				// T03 修订(用户 2026-09-16):子面板(停靠形态)拖出只在**主窗口内浮动**,
				// 不创建 OS 窗口 —— 独立 OS 窗口只属于声明为 Independent 的面板
				// (Widget Gallery / Input Map)。因此这里全部用主窗口客户区坐标。
				Wui::WuiRect source { m_LastDragPos.x - 40.0f, m_LastDragPos.y - 12.0f, 480.0f, 320.0f };
				if (const auto remembered = m_LastFloatRects.find(m_DragPanel); remembered != m_LastFloatRects.end())
				{
					source.W = remembered->second.W;
					source.H = remembered->second.H;
				}
				else
				{
					// 首次拖出:沿用原停靠区的尺寸作为初始浮动尺寸。
					std::vector<std::pair<Wui::PanelId, Wui::WuiRect>> rects;
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
				// 初始位置夹在编辑区里,标题栏必须可见(与 RenderFloatWindow 的约束一致)。
				source.W = std::min(source.W, std::max(320.0f, viewport.x - 16.0f));
				source.H = std::min(source.H, std::max(240.0f, viewport.y - editorTop - 16.0f));
				source.X = std::max(8.0f, std::min(source.X, std::max(8.0f, viewport.x - source.W - 8.0f)));
				source.Y = std::max(editorTop, std::min(source.Y, std::max(editorTop, viewport.y - source.H - 8.0f)));
				const std::string before = m_Layout.Serialize();
				if (m_Layout.Float(m_DragPanel, source))
				{
					RecordDockChange(ctx, "float", m_DragPanel, before);
					// 拖动期间面板跟随光标:抓取点固定在标题栏左侧(贴近原标签位置)。
					m_MovingFloat = m_DragPanel;
					m_FloatGrabOffset = { 40.0f, 12.0f };
					m_LastFloatRects[m_DragPanel] = source;
					// P3-1①:拖出已经完成,消费掉"本次拖拽的起手标签"标记 —— 同一个拖拽里
					// 面板若又回到停靠树(例如点浮窗 ✕ 收回停靠位),不允许再浮出一次。
					m_TabDragPanel.clear();
				}
				else if (std::getenv("WLD_TRACE_UI"))
				{
					WLD_CORE_WARN("[float] drag-out float() rejected for '{0}'", m_DragPanel);
				}
			}
			else if (std::getenv("WLD_TRACE_UI"))
			{
				// 诊断:拖拽已激活但没有走拖出分支(供脚本/人工排查用,按面板去重打印)。
				static std::string lastSkipped;
				if (lastSkipped != m_DragPanel)
				{
					lastSkipped = m_DragPanel;
					WLD_CORE_INFO("[float] drag-out skipped: panel={0} consumed={1} cooldown={2} contained={3} tabDrag={4}",
						m_DragPanel, dropConsumed ? 1 : 0, m_AttachCooldownFrames,
						m_Layout.Contains(m_DragPanel) ? 1 : 0, m_TabDragPanel);
				}
			}
			else if (!m_DragPanel.empty() && m_Layout.Contains(m_DragPanel))
			{
				// 拖出条件未满足:保持原状(用于人工排查,不打印高频日志)。
			}
		}
		else if (leftButtonDown && ctx.IsDragActive(nullptr))
		{
			// 其他类型的拖拽(file: 等)不参与面板浮动。
			m_DragPanel.clear();
			m_TabDragPanel.clear();
		}
		// P3-1① 修订(用户 2026-09-20 报"子栏不能拖拽了"):出口收口只在**物理左键已经抬起**
		// 时执行。WuiContext 的拖拽是两段式:按下沿的 BeginDrag 只进入 pending(记下
		// m_DragPressPos),移动超过 4px 后(WuiContext::EndFrame / 下一次 BeginDrag)才置
		// m_Dragging —— 期间 IsDragActive() 恒为 false。而 m_TabDragPanel / m_MovingFloat 在
		// **按下沿当帧**就已经写入;若这时按"拖拽结束"收口,ClearPanelDragIdentity() 会清掉
		// 起手标记、ctx.EndDrag() 会直接撤销 pending —— 面板再也进不了拖拽(实测表现:按住
		// 标签拖动,面板完全不动)。pending 期间这些身份成员是拖拽必需的,所以收口条件加上
		// !leftButtonDown:松手 / 释放事件丢失(由 WuiInputCollector::SyncButtonsWithSystem
		// 补发释放)时左键已抬起,收口照常发生,P3-1 的"拖出后一次点 ✕ 就关闭"不受影响。
		else if (!leftButtonDown
			&& (!m_DragPanel.empty() || !m_MovingFloat.empty() || !m_TabDragPanel.empty()))
		{
			// 拖拽结束(松手 / 释放事件丢失):清"拖拽身份"成员 —— 拖拽起点标记残留会让
			// 已经收回停靠的面板立刻再次浮出。**落点成员必须留到下一帧**:WuiContext 下一帧
			// 才交付 AcceptDrop,落位用的就是这一帧武装好的目标;这里清掉会把落点整个吃掉。
			ClearPanelDragIdentity();
			// ctx 侧的拖拽结束由 WuiContext::EndFrame 负责:它要把"本帧已武装的落点"转成
			// 可消费的 m_DropAccepted(见 WuiContext.cpp 的 release 分支)。在"释放沿"这一帧
			// 调 EndDrag 会把这次落点整个吃掉(拖回停靠位失效),所以只在 ctx 已经不在拖拽时
			// 清残留 payload。
			if (!ctx.IsDragActive(nullptr))
				ctx.EndDrag();
		}

		// 浮动面板绘制在停靠区之上、菜单/模态之下。
		RenderFloating(ctx);

		// 菜单栏最后绘制:其弹出面板需要盖在所有停靠面板之上。
		DrawMenuBar(ctx);
		// 挂靠栏:横条形式(排在菜单栏下方),独立窗口可挂靠至此。
		DrawAttachBar(ctx);

		// D10-11/D10-15:模态绘制前解除遮挡(对话框自身要能命中);画完再清一次,
		// 确保本帧结束时不留 blocker(下一帧开帧也会清,这里是双保险)。
		if (shellModalOpen)
			Wui::EndModalInputBlock(ctx);
		else
			ctx.ClearHoverBlockers();
		DrawModals(ctx);
		ctx.ClearHoverBlockers();

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

		// 遍历结束后再执行标签关闭请求:这样"关闭组内最后一个标签"引起的塌缩
		// 不会打断正在进行的渲染遍历(修"关闭 Saves/Levels 等标签崩溃")。
		for (const Wui::PanelId& panel : m_PendingPanelCloses)
		{
			const std::string before = m_Layout.Serialize();
			if (m_Layout.RemoveTab(panel))
				RecordDockChange(ctx, "close", panel, before);
		}
		m_PendingPanelCloses.clear();

		// P3-1①:挂靠标签拖拽(不经过 WuiContext)的收口 —— 左键已经抬起,而
		// DrawAttachBar 这一帧没收到释放沿(释放落在别的窗口/窗口外)时,残留会让顶栏
		// 一直停在"正在拖标签"的状态。正常路径上 DrawAttachBar 已消费并清空,这里不会再触发;
		// 只收口"已经在拖"的状态:m_AttachTagPress 要留给 DrawAttachBar 的"单击=切换视图"
		// 释放沿消费(脚本注入的点击没有物理按键,不能在这里被判成残留)。
		if (!leftButtonDown && !m_AttachTagDrag.empty())
		{
			m_AttachTagPress.clear();
			m_AttachTagDrag.clear();
		}

		// W9-2:本帧(含所有独立窗口)结束时的文本焦点快照。UI 帧开始时各窗口的登记
		// 已被 BeginFrame 清空,所以帧内 Ctrl+Z/Y 判定必须用"上一帧结束"的这份状态。
		// P4-UX4:悬停提示最后画 —— 面板/模态都已登记完,这里统一画到 overlay 层。
		DrawStatusBar(ctx, statusBar);
		Wui::DrawTooltip(ctx, m_Theme);
		m_TextFocusLatched = Wui::WuiTextFocus::Get().Active();
	}

	// P4-UX6:状态栏 —— 一眼看到"当前场景有没有改、选中了什么、跑在哪个后端、多少帧"。
	// 全部取自既有状态(文档/选择/Input().FPS),不做任何额外计算或分配。
	void EditorShell::DrawStatusBar(Wui::WuiContext& ctx, const Wui::WuiRect& rect)
	{
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, m_Theme.PanelHeader, 0.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { rect.X, rect.Y, rect.W, 1.0f },
			m_Theme.Border, 0.0f });

		EditorDocument& document = m_Editor.GetDocument();
		const std::string sceneName = document.HasPath()
			? document.GetPath().filename().string() : std::string("Untitled");
		std::string left = std::string(Wui::Tr("status.scene", "Scene")) + ": " + sceneName;
		if (document.IsDirty())
			left += " *";
		Entity selected = GetSelectedEntity();
		left += "   |   " + std::string(Wui::Tr("status.selection", "Selection")) + ": ";
		if (selected && selected.HasComponent<TagComponent>())
			left += selected.GetComponent<TagComponent>().Tag;
		else
			left += Wui::Tr("status.selection.none", "none");
		if (!m_ActiveWindowTag.empty())
			left += "   |   " + std::string(Wui::Tr("status.view", "View")) + ": " + PanelTitle(m_ActiveWindowTag);

		const float fps = ctx.Input().FPS;
		char right[96] = {};
		std::snprintf(right, sizeof(right), "%s   %.1f FPS (%.1f ms)", Renderer::GetBackendName().c_str(),
			static_cast<double>(fps), fps > 0.0f ? 1000.0 / static_cast<double>(fps) : 0.0);
		const std::string rightText = right;
		const float rightWidth = ctx.MeasureTextWidth(rightText, 12.0f);
		const float textY = rect.Y + (rect.H - 14.0f) * 0.5f;
		Wui::Label(ctx, { rect.X + 10.0f, textY }, left, m_Theme.TextMuted, 12.0f);
		Wui::Label(ctx, { rect.X + rect.W - rightWidth - 10.0f, textY }, rightText,
			m_Theme.TextMuted, 12.0f);
		// 无障碍:脚本/AI 通道可直接读整条状态(不依赖像素)。
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId("shell.status");
		node.Window = "main";
		node.Panel = "shell";
		node.Kind = "status";
		node.Label = "editor status bar";
		node.Value = left + "   |   " + rightText;
		node.Rect = rect;
		node.Enabled = false;
		node.Interactive = false;
		node.Visible = true;
		Wui::WuiAccessibility::Get().Register(node);
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
		// 位置记忆:记下每个停靠标签所在组的首个面板(菜单重新打开该面板时回到这一组)。
		if (!node.Panels.empty())
			for (const std::string& panel : node.Panels)
				m_LastDockAnchors[panel] = node.Panels.front();
		// 停靠标签栏统一走组件(WuiChrome::DockTabBar),主窗口与独立窗口外观/交互一致。
		std::vector<Wui::DockTab> tabs;
		tabs.reserve(node.Panels.size());
		for (size_t i = 0; i < node.Panels.size(); ++i)
			tabs.push_back({ Wui::HashId(("tab." + node.Panels[i]).c_str()), PanelTitle(node.Panels[i]), i == node.Active });
		const Wui::DockTabBarResult tabResult = Wui::DockTabBar(ctx, { area.X, area.Y, area.W, tabH }, tabs, m_Theme);

		if (tabResult.Clicked >= 0 && static_cast<size_t>(tabResult.Clicked) < node.Panels.size())
		{
			m_Layout.Activate(node.Panels[tabResult.Clicked]);
			// 单纯点击标签不是拖拽:清掉可能残留的拖拽来源记录。
			m_TabDragPanel.clear();
		}
		if (tabResult.Closed >= 0 && static_cast<size_t>(tabResult.Closed) < node.Panels.size())
		{
			// 只登记请求:此刻正在遍历停靠树,直接删标签会让本组(乃至父级分栏)
			// 塌缩,RenderTabs/RenderSplit 手里的 node/children 引用立即失效。
			// 真正删除在 OnRender 末尾统一执行(见 m_PendingPanelCloses)。
			m_PendingPanelCloses.push_back(node.Panels[tabResult.Closed]);
		}
		if (tabResult.DragStart >= 0 && static_cast<size_t>(tabResult.DragStart) < node.Panels.size())
		{
			const std::string& panel = node.Panels[tabResult.DragStart];
			// P3-1①:拖拽只能在**按下沿**起手。DockTabBar 的 DragStart 是"按住即报",
			// 若照单全收,一次点击在"面板刚回到停靠树、标签正好画在光标下、按键还没抬"时
			// (典型场景:点浮窗 ✕ → 面板收回停靠位)会被当成新的拖拽,面板立刻被再次浮出 ——
			// 这正是"第一次点 ✕ 被'重新浮出'吃掉"的机制。独立窗口的标签早就是按下沿起手
			// (见 FloatWindowHost::RenderTabBar),这里补齐同一条规则。
			if (ctx.Input().MouseClicked[0])
			{
				ctx.BeginDrag(Wui::HashId(("tab." + panel).c_str()), "panel:" + panel);
				// 记录本次拖拽的真实来源:只在还没有来源时记一次。拖动过程中经过别的标签页时
				// DockTabBar 仍会报 DragStart,若覆盖会把真实来源记错 —— 拖出分支的
				// "m_TabDragPanel == m_DragPanel" 守卫随即拒绝,表现是面板拖不出来。
				if (m_TabDragPanel.empty())
					m_TabDragPanel = panel;
			}
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
			// 中心区(面板主体或标签栏)= 合并进该组的标签页(用户 2026-09-16:
			// 拖到另一个子面板上就应该变成同组标签,原来只认 24px 标签栏,很难命中)。
			ctx.DropTarget(area, "panel:"); // 武装落点:仅面板拖拽在此生效
			m_DropZone = targetZone;
			Wui::WuiRect zone = area;
			if (m_DropZone == Wui::DropZone::Left) zone.W = area.W * 0.25f;
			else if (m_DropZone == Wui::DropZone::Right) { zone.X = area.X + area.W * 0.75f; zone.W = area.W * 0.25f; }
			else if (m_DropZone == Wui::DropZone::Top) zone.H = area.H * 0.25f;
			else if (m_DropZone == Wui::DropZone::Bottom) { zone.Y = area.Y + area.H * 0.75f; zone.H = area.H * 0.25f; }
			else zone.H = tabH; // 中心落点:高亮该组标签栏(它会变成这里的一个标签页)
			// 落区预览延迟到 RenderFloating 里画(浮动面板之上),否则会被拖动中的面板盖住。
			m_DropPreviewRect = zone;
			m_DropPreviewActive = true;
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
		// 无障碍树:此后登记的控件归属该面板(ui.tree/state.dump 靠它区分面板)。
		Wui::WuiAccessibility::Get().SetPanel(id);
		ctx.SetPanelId(id);
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
			// P3-1②:Main 标签同样登记(稳定 id `shell.attach.main`)—— 脚本可以在
			// ui.attach 把视图切到某个附加窗口之后,再读/点这一枚标签切回主界面。
			RegisterAttachNode("main", tab, active ? "active" : "inactive", "Main", true);
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
			// P3-1②:同一 id 在浮动态由下面的浮窗分支登记(value=floating),这里登记附加态。
			RegisterAttachNode(panel, tab, "attached", PanelTitle(panel), true);
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

		// 同一窗口的其它标签页:顶栏只给窗口一枚 chip(拖出=整个窗口),但它们的形态
		// 也要能从无障碍树读到 —— 注册成只读节点(Interactive=false,共用同一 chip 矩形)。
		for (const AttachTagHit& hit : tagHits)
		{
			FloatWindowHost* host = FindFloatHost(hit.Panel);
			if (!host)
				continue;
			for (const std::string& panel : host->Panels())
			{
				if (panel == hit.Panel)
					continue;
				RegisterAttachNode(panel, hit.Rect, "attached-tab", PanelTitle(panel), false);
			}
		}

		// P3-1②:可见的独立窗口 = 浮动态。节点与顶栏标签共用 id(`shell.attach.<panel>`),
		// value=floating;rect 由窗口屏幕矩形换算到主窗口客户区,只用于"读状态",因此
		// Interactive=false(点它不应该落到主窗口上,也不参与 ui.invoke)。
		{
			int mainX = 0, mainY = 0;
			if (Application::HasInstance())
				Application::Get().GetWindow().GetPosition(&mainX, &mainY);
			for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
			{
				if (host->IsHidden())
					continue;
				const Wui::WuiRect screen = host->ScreenRect();
				const Wui::WuiRect local { screen.X - static_cast<float>(mainX),
					screen.Y - static_cast<float>(mainY), screen.W, screen.H };
				for (const std::string& panel : host->Panels())
					RegisterAttachNode(panel, local, "floating", PanelTitle(panel), false);
			}
		}

		if (!closeRequest.empty())
		{
			// × = 关闭:隐藏该窗口的面板(可从 Window 菜单重新打开),并把标签移出栏。
			// 一窗口一枚标签:关掉的是整个窗口,该窗口所有标签页的记录一起摘掉。
			std::vector<std::string> closingPanels;
			if (FloatWindowHost* host = FindFloatHost(closeRequest))
				closingPanels = host->Panels();
			if (closingPanels.empty())
				closingPanels.push_back(closeRequest);
			CloseFloatWindow(closeRequest, true, &ctx);
			for (const std::string& id : closingPanels)
			{
				m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), id),
					m_AttachedPanels.end());
				if (m_ActiveWindowTag == id)
					m_ActiveWindowTag.clear();
			}
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
				const std::vector<std::string> hostPanels = host->Panels();
				int mainX = 0, mainY = 0;
				if (Application::HasInstance())
					Application::Get().GetWindow().GetPosition(&mainX, &mainY);
				host->SetScreenPosition(static_cast<float>(mainX) + ctx.Input().MousePos.x - 60.0f,
					static_cast<float>(mainY) + ctx.Input().MousePos.y - 12.0f);
				// 拖出来的是**这个窗口**,窗口显示用户拖的那一页(标签页可能被切过)。
				host->ActivatePanel(dragged);
				host->SetHidden(false);
				// 整窗离槽:把该窗口里所有标签页的附加记录一次性摘干净(不留下悬空 chip)。
				for (const std::string& id : hostPanels)
				{
					m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), id),
						m_AttachedPanels.end());
					if (m_ActiveWindowTag == id)
						m_ActiveWindowTag.clear();
				}
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
		// 注意:viewport 是**设计单位**(= 物理像素 / UiScale),而这里要和 GetCursorPos /
		// 窗口屏幕矩形(都是物理像素)比较 —— 必须乘回 UiScale,否则 UI 缩放 1.3 时
		// 命中区只有真实栏高的 77%,"拖到栏上"会时灵时不灵。
		const float uiScale = Wui::UiScale();
		m_AttachSlotScreenRect = { 0, 0.0f, viewport.x * uiScale, m_AttachBarHeight * uiScale };
		int windowX = 0, windowY = 0;
		if (Application::HasInstance())
			Application::Get().GetWindow().GetPosition(&windowX, &windowY);
		m_AttachSlotScreenRect.X += static_cast<float>(windowX);
		m_AttachSlotScreenRect.Y += static_cast<float>(windowY);

		// 每个独立窗口渲染自己的 OS 窗口(含标签栏);窗口被关闭 = 隐藏其全部面板。
		// 标签栏 x 只登记关闭请求,统一在遍历结束后处理,避免边遍历边改 m_FloatHosts。
		std::vector<std::string> closeRequests;
		// 收集新发起的标签拖拽(跨窗口附加);同帧只接受一个。
		// 独立窗口标签被拖过阈值 → 交给系统移动循环(阻塞到松手,见 PerformIndependentWindowDrag)。
		for (const std::unique_ptr<FloatWindowHost>& candidate : m_FloatHosts)
		{
			if (const std::string drag = candidate->TakePendingTabDrag(); !drag.empty())
			{
				PerformIndependentWindowDrag(drag);
				break;
			}
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

			// 位置记忆写回布局(拖动由系统移动循环负责,这里只记录最终矩形)。
			const std::string windowKey = host.Panels().front();
			m_LastFloatScreenRects[windowKey] = rect;
			++i;
		}
		for (const std::string& panel : closeRequests)
			HideFloatPanel(panel, &ctx);
		// 挂靠栏高亮只可能由"正在拖窗口"点亮(P4-UX9 起拖动走系统移动循环,循环期间不渲染,
		// 松手后由 PerformIndependentWindowDrag 收口)—— 空闲时一律熄灭,避免残留。
		m_AttachSlotHighlight = false;
		// 停靠形态的"临时浮动"面板:在主窗口内绘制(OS 窗口只属于 Independent 面板)。
		// 独立窗口渲染会把当前 GL 上下文切到各自窗口,先恢复主窗口上下文。
		if (Application::HasInstance())
			Application::Get().GetWindow().MakeCurrent();
		for (size_t i = 0; i < m_Layout.Floating.size(); ++i)
		{
			const std::string panel = m_Layout.Floating[i].Panel;
			if (IsIndependentPanel(panel))
				continue; // 独立面板有自己的 OS 窗口
			bool closed = false;
			RenderFloatWindow(ctx, m_Layout.Floating[i], &closed);
			if (closed)
			{
				// 停靠形态的浮动窗口关闭 = 回停靠位(D3),不是隐藏面板。
				HideFloatPanel(panel, &ctx);
				break; // 容器已改变,下一帧继续绘制其余窗口
			}
		}
		// 置顶在绘制结束后应用,避免遍历中修改容器。
		if (!m_BringFloatFront.empty())
		{
			m_Layout.BringFloatToFront(m_BringFloatFront);
			m_BringFloatFront.clear();
		}
		// 落点预览画在所有浮动面板之上:拖动中的面板正好盖在目标上。
		if (m_DropPreviewActive)
			Wui::DropZoneOverlay(ctx, m_DropPreviewRect, 0.30f, 3.0f);
		// 跨窗口拖拽的目标命中与落点(在窗口渲染之后执行,便于统一改容器)。
		// 独立窗口渲染会把 GL 上下文切到各自窗口,这里恢复主窗口上下文,
		// 否则主窗口后续的呈现/交换会作用在错误的上下文上(表现为主窗口不再刷新)。
		if (Application::HasInstance())
			Application::Get().GetWindow().MakeCurrent();
	}

	// 窗口内浮动面板(停靠形态面板拖出后的形态)的绘制与交互:
	// 标题栏拖动 = 移动(拖动载荷仍是 "panel:",拖到停靠落点上即回停靠);
	// 右下角 = 缩放;左上 × = 关闭浮动窗口(停靠形态 = 回停靠位,D3)。
	// 独立窗口(Independent)不走这条路径,它们由 FloatWindowHost 的 OS 窗口渲染。
	void EditorShell::RenderFloatWindow(Wui::WuiContext& ctx, Wui::DockFloat& window, bool* closed)
	{
		const float titleH = 24.0f;
		const glm::vec2 viewport = ctx.ViewportSize();
		const float topLimit = 26.0f + m_AttachBarHeight;
		Wui::WuiRect& rect = window.Rect;

		// 视口约束:窗口不能完全跑出编辑区,标题栏必须可见。
		rect.W = std::max(240.0f, std::min(rect.W, std::max(240.0f, viewport.x - 16.0f)));
		rect.H = std::max(160.0f, std::min(rect.H, std::max(160.0f, viewport.y - topLimit - 16.0f)));
		rect.X = std::max(8.0f, std::min(rect.X, std::max(8.0f, viewport.x - rect.W - 8.0f)));
		rect.Y = std::max(topLimit, std::min(rect.Y, std::max(topLimit, viewport.y - rect.H - 8.0f)));
		if (std::getenv("WLD_TRACE_UI"))
		{
			// 诊断/自动化:窗口内浮动面板的实际矩形(客户区坐标),供脚本点击标题栏与关闭按钮。
			static int traced = 0;
			if (traced < 60)
			{
				++traced;
				WLD_CORE_INFO("[float] in-window '{0}' rect=({1},{2},{3},{4})",
					window.Panel, rect.X, rect.Y, rect.W, rect.H);
			}
		}

		ctx.PushOverlay();
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, rect, m_Theme.PanelBg, 5.0f });
		ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, rect, m_Theme.Border, 5.0f, 1.0f });
		const Wui::WuiRect title { rect.X, rect.Y, rect.W, titleH };
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, title, m_Theme.PanelHeader, 5.0f });
		Label(ctx, { title.X + 10.0f, title.Y + 4.0f }, PanelTitle(window.Panel), m_Theme.Text, 14.0f);

		// 关闭按钮
		const Wui::WuiRect close { title.X + title.W - 22.0f, title.Y + 5.0f, 14.0f, 14.0f };
		const bool overClose = ctx.IsHovered(close);
		if (overClose)
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, close, m_Theme.ButtonHover, 2.0f });
		Label(ctx, { close.X + 3.0f, close.Y - 2.0f }, "x", m_Theme.TextMuted, 13.0f);
		const bool closeClicked = ctx.IsClicked(close);

		// 标题栏拖动:置顶 + 跟随鼠标;松手落在停靠落点上则由外壳的落位逻辑回停靠。
		if (!closeClicked && !overClose && ctx.IsClicked(title))
		{
			m_BringFloatFront = window.Panel;
			m_MovingFloat = window.Panel;
			m_FloatGrabOffset = ctx.Input().MousePos - glm::vec2 { rect.X, rect.Y };
			m_FloatChangeBefore = m_Layout.Serialize();
			ctx.BeginDrag(Wui::HashId(("float." + window.Panel).c_str()), "panel:" + window.Panel);
		}
		if (m_MovingFloat == window.Panel && ctx.IsDragActive(nullptr))
		{
			rect.X = std::max(8.0f, std::min(ctx.Input().MousePos.x - m_FloatGrabOffset.x,
				std::max(8.0f, viewport.x - rect.W - 8.0f)));
			rect.Y = std::max(topLimit, std::min(ctx.Input().MousePos.y - m_FloatGrabOffset.y,
				std::max(topLimit, viewport.y - rect.H - 8.0f)));
			ctx.SetCursor(Wui::WuiCursor::Hand);
		}

		// 右下角缩放
		const Wui::WuiRect grip { rect.X + rect.W - 16.0f, rect.Y + rect.H - 16.0f, 16.0f, 16.0f };
		if (ctx.IsHovered(grip))
		{
			ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, grip, m_Theme.ButtonHover, 3.0f });
			ctx.SetCursor(Wui::WuiCursor::ResizeEW);
		}
		if (ctx.IsClicked(grip))
		{
			m_BringFloatFront = window.Panel;
			m_FloatResize = window.Panel;
			m_FloatResizeStart = ctx.Input().MousePos;
			m_FloatResizeRect = rect;
			m_FloatChangeBefore = m_Layout.Serialize();
		}
		if (m_FloatResize == window.Panel && ctx.Input().MouseDown[0])
		{
			const glm::vec2 delta = ctx.Input().MousePos - m_FloatResizeStart;
			rect.W = std::max(240.0f, m_FloatResizeRect.W + delta.x);
			rect.H = std::max(160.0f, m_FloatResizeRect.H + delta.y);
		}
		if (m_FloatResize == window.Panel && ctx.Input().MouseReleased[0])
		{
			m_FloatResize.clear();
			if (!m_FloatChangeBefore.empty())
			{
				RecordDockChange(ctx, "float-resize", window.Panel, m_FloatChangeBefore);
				m_FloatChangeBefore.clear();
			}
		}
		// 尺寸记忆:下次拖出沿用用户调好的大小(位置按当次拖拽点重新计算)。
		m_LastFloatRects[window.Panel] = { 0.0f, 0.0f, rect.W, rect.H };

		// 自动化钩子(开发验证):WLD_FLOAT_RECT_FILE=<路径> 时把浮动面板的客户区矩形
		// 写进该文件(矩形变化才写),供脚本点击标题栏/关闭按钮。
		if (const char* rectFile = std::getenv("WLD_FLOAT_RECT_FILE"))
		{
			char buffer[192];
			std::snprintf(buffer, sizeof(buffer), "panel=%s\nx=%.1f\ny=%.1f\nw=%.1f\nh=%.1f\n",
				window.Panel.c_str(), rect.X, rect.Y, rect.W, rect.H);
			static std::string lastWritten;
			if (lastWritten != buffer)
			{
				lastWritten = buffer;
				std::ofstream(rectFile, std::ios::trunc) << buffer;
			}
		}

		// 内容区(标题栏之下)
		const Wui::WuiRect body { rect.X + 1.0f, rect.Y + titleH, rect.W - 2.0f, rect.H - titleH - 1.0f };
		RenderPanelContent(ctx, window.Panel, body);
		ctx.PopOverlay();

		if (closeClicked && closed)
			*closed = true;
	}

	// P4-UX9:拖动独立窗口(在标签栏/空白区按下并越过阈值后进入)。
	//
	// 手感设计(用户 2026-09-20:"独立窗口拖拽手感差,鼠标滑动一快就会出现偏移"):
	//   ① 窗口移动交给**系统移动循环**(SC_MOVE):系统按输入频率移动窗口,与渲染帧率无关
	//      —— 本引擎 Debug 下一帧 50ms,"每帧轮询光标"必然发飘,再怎么调都追不上;
	//      它同时自带 Esc 取消与系统吸附,是 Windows 上拖标题栏的工业标准做法。
	//   ② 进入循环前把窗口"预置"到 光标 - 按下瞬间的抓取偏移:补偿"按下 → 识别到拖动"
	//      之间已经发生的位移。否则系统会把那段位移吸收进抓取偏移,快速甩动时窗口不跟手。
	//   ③ 拖动期间光标进入挂靠栏 → 窗口被压到栏下方(SetSystemDragParkZone):
	//      "窗口停在栏下"就是"松手即挂靠"的可见提示,目标不会被窗口自己挡住。
	//   ④ 松手按真实落点判定:挂靠栏上 → 整窗挂靠;Esc → 回到拖动前的位置(取消);
	//      其它位置 → 就停在那里(位置由 OnRender 里既有的写回逻辑进布局)。
	void EditorShell::PerformIndependentWindowDrag(const std::string& panel)
	{
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host)
			return;
		Window* window = host->NativeWindow();
		if (!window)
			return;

		const glm::vec2 grab = host->TakePendingTabDragGrab();
		const Wui::WuiRect startRect = host->ScreenRect();
		POINT cursor { 0, 0 };
		GetCursorPos(&cursor);

		int mainX = 0, mainY = 0;
		float mainW = static_cast<float>(cursor.x + 1280);
		if (Application::HasInstance())
		{
			Application::Get().GetWindow().GetPosition(&mainX, &mainY);
			mainW = static_cast<float>(Application::Get().GetWindow().GetWidth());
		}
		const float barPixels = m_AttachBarHeight * Wui::UiScale();

		window->SetPosition(static_cast<int>(static_cast<float>(cursor.x) - grab.x),
			static_cast<int>(static_cast<float>(cursor.y) - grab.y));
		window->SetSystemDragParkZone(
			{ static_cast<float>(mainX), static_cast<float>(mainY), mainW, barPixels },
			static_cast<float>(mainY) + barPixels + 6.0f);
		host->SetTabDragActive(true);
		window->BeginSystemDrag();          // 阻塞:系统移动循环,回到这里就是松手
		window->SetSystemDragParkZone({ 0.0f, 0.0f, 0.0f, 0.0f }, 0.0f);
		host->SetTabDragActive(false);

		if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
		{
			// Esc = 取消(系统只保证回到循环起点,也就是预置后的位置;这里再放回按下前的位置)。
			host->SetScreenPosition(startRect.X, startRect.Y);
			WLD_CORE_INFO("[float] drag cancelled by Esc: {0}", panel);
			return;
		}

		POINT released { 0, 0 };
		GetCursorPos(&released);
		const bool overAttachBar = IsIndependentPanel(panel)
			&& m_AttachSlotScreenRect.W > 0.0f
			&& static_cast<float>(released.x) >= m_AttachSlotScreenRect.X
			&& static_cast<float>(released.x) <= m_AttachSlotScreenRect.X + m_AttachSlotScreenRect.W
			&& static_cast<float>(released.y) >= m_AttachSlotScreenRect.Y
			&& static_cast<float>(released.y) <= m_AttachSlotScreenRect.Y + m_AttachSlotScreenRect.H;
		// 落点诊断(拖拽是模态循环,出问题时日志是唯一现场)。
		WLD_CORE_INFO("[float] drag end: panel={0} released=({1},{2}) slot=({3:.0f},{4:.0f},{5:.0f},{6:.0f}) attach={7}",
			panel, released.x, released.y, m_AttachSlotScreenRect.X, m_AttachSlotScreenRect.Y,
			m_AttachSlotScreenRect.W, m_AttachSlotScreenRect.H, overAttachBar ? 1 : 0);
		if (overAttachBar)
		{
			AttachIndependentWindowToSlot(panel);
			return;
		}
		if (m_Ctx)
			m_Ctx->RecordOp("float", "move", panel, "");
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
			// P4-UX9:只复用**空窗**(面板全部关掉了的"壳")。以前会把新面板塞进一个还挂着
			// 别的面板的隐藏窗口里,于是"一个 OS 窗口 + 两枚顶栏标签",拖出左侧那枚会把整个
			// 窗口(含另一个面板)一起拔出来,另一枚标签的状态就悬空了(用户 2026-09-20 复现)。
			if (!host->IsHidden() || !host->Panels().empty())
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

	// ---- AI 控制通道 ----

	bool EditorShell::AiTogglePanel(const std::string& panel)
	{
		if (panel.empty() || !IsDeclaredPanel(panel))
			return false;
		// 材质面板是动态实例:先按 id 建出面板对象,再走菜单同一条开关路径。
		if (panel.rfind("material:", 0) == 0)
		{
			EnsureMaterialPanelFromId(panel);
			// 与 OpenMaterialEditor 同一条默认尺寸:材质面板的窄布局会把贴图下拉挤到窗口外
			// (实测 480x340 时 Albedo 组合框在 y=782 → 用户根本看不到)。
			if (!m_Layout.FindFloatMemory(panel, nullptr))
				m_Layout.FloatMemory.push_back({ panel, Wui::WuiRect { 200.0f, 170.0f, 760.0f, 470.0f } });
		}
		if (panel.rfind(kModelPanelPrefix, 0) == 0)
		{
			EnsureModelPanelFromId(panel);
			if (!m_Layout.FindFloatMemory(panel, nullptr))
				m_Layout.FloatMemory.push_back({ panel, Wui::WuiRect { 220.0f, 160.0f, 620.0f, 660.0f } });
		}
		// W9-2:动态脚本面板 —— 未打开 → 走 OpenScriptEditor(默认附加到主窗口);
		// 已打开(附加标签或可见独立窗口)→ 走菜单同一条开关路径关闭。
		if (panel.rfind(kScriptPanelPrefix, 0) == 0)
		{
			EnsureScriptPanelFromId(panel);
			if (!m_Ctx)
				return false;
			const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
			FloatWindowHost* host = FindFloatHost(panel);
			const bool visibleWindow = host && !host->IsHidden();
			if (attached != m_AttachedPanels.end() || visibleWindow)
				TogglePanel(*m_Ctx, panel);
			else
				OpenScriptEditorNow(panel.substr(std::strlen(kScriptPanelPrefix)));
			return true;
		}
		if (!m_Ctx)
			return false;
		TogglePanel(*m_Ctx, panel);
		return true;
	}

	// P3-1②:脚本化"分离 / 挂回"。与顶部挂靠栏拖拽、窗口菜单走同一对既有路径:
	//   分离 = OpenIndependentPanel(复用已隐藏的窗口 / 必要时新建),附加态先把顶栏标签摘掉;
	//   挂回 = AttachIndependentWindowToSlot(OS 窗口隐藏 + 顶栏出现切换标签)。
	// 已处于目标状态时幂等,可读结果写进 message(供脚本直接断言,不必解析布局 JSON)。
	bool EditorShell::AiDetachPanel(const std::string& panel, std::string* message)
	{
		auto fail = [message](const std::string& text)
		{
			if (message)
				*message = text;
			return false;
		};
		if (panel.empty() || !IsDeclaredPanel(panel))
			return fail("unknown panel '" + panel + "'");
		// 形态规则(T03):只有声明为独立窗口的面板才有"附加态/浮动态";停靠形态面板的
		// 拖出是主窗口内的临时浮动,不在这两个命令的语义里(用 ui.open/拖拽改它的位置)。
		if (!IsIndependentPanel(panel))
			return fail("panel '" + panel + "' is a docked panel; ui.detach/ui.attach only apply to independent windows");

		const char* const state = PanelStateLabel(panel);
		if (std::strcmp(state, "floating") == 0)
		{
			// 幂等:已经是目标状态(真 OS 窗口)时不做任何事,只回报现状。
			if (message)
				*message = "already floating: " + panel + " (independent OS window)";
			return true;
		}
		const bool wasAttached = std::strcmp(state, "attached") == 0;
		const std::string before = m_Layout.Serialize();
		OpenIndependentPanel(panel); // 复用已隐藏的窗口;从未打开过则新建(位置走 FloatRectFor 记忆)
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host || host->IsHidden())
		{
			// 窗口没建起来:附加态/标签保持原样,命令可重试(不制造"既没标签也没窗口"的半状态)。
			return fail("cannot show independent window for '" + panel + "'");
		}
		if (wasAttached)
		{
			// 摘掉顶栏标签(与顶栏 ✕ 之后的清理同一份状态):窗口本体保持不变。
			m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel),
				m_AttachedPanels.end());
			if (m_ActiveWindowTag == panel)
				m_ActiveWindowTag.clear();
		}
		if (m_Ctx)
			RecordDockChange(*m_Ctx, "detach", panel, before);
		// 与顶栏拖出同一条操作记录(category/action/target 口径一致)。
		if (m_Ctx)
			m_Ctx->RecordOp("float", "detach", panel, wasAttached ? "attached" : "opened");
		const Wui::WuiRect rect = host->ScreenRect();
		if (message)
		{
			std::ostringstream text;
			text << (wasAttached ? "detached " : "opened floating ") << panel
				<< " (independent window at " << static_cast<int>(rect.X) << "," << static_cast<int>(rect.Y)
				<< " " << static_cast<int>(rect.W) << "x" << static_cast<int>(rect.H) << ")";
			*message = text.str();
		}
		return true;
	}

	bool EditorShell::AiAttachPanel(const std::string& panel, std::string* message)
	{
		auto fail = [message](const std::string& text)
		{
			if (message)
				*message = text;
			return false;
		};
		if (panel.empty() || !IsDeclaredPanel(panel))
			return fail("unknown panel '" + panel + "'");
		if (!IsIndependentPanel(panel))
			return fail("panel '" + panel + "' is a docked panel; ui.detach/ui.attach only apply to independent windows");

		if (std::strcmp(PanelStateLabel(panel), "attached") == 0)
		{
			// 幂等:已经是目标状态(顶栏标签 + 隐藏的 OS 窗口)时不做任何事,只回报现状。
			if (message)
				*message = "already attached: " + panel + " (top-bar tag active, OS window hidden)";
			return true;
		}
		const std::string before = m_Layout.Serialize();
		if (!FindFloatHost(panel))
			OpenIndependentPanel(panel); // 未打开过:先按独立窗口建出,再走同一条挂靠路径
		AttachIndependentWindowToSlot(panel);
		if (!FindFloatHost(panel) || std::strcmp(PanelStateLabel(panel), "attached") != 0)
			return fail("cannot attach panel '" + panel + "'");
		if (m_Ctx)
			RecordDockChange(*m_Ctx, "attach", panel, before);
		if (message)
			*message = std::string("attached ") + panel + " (independent window hidden; top-bar tag = "
				+ PanelTitle(panel) + ")";
		return true;
	}

	bool EditorShell::AiRequestFloatCapture(const std::string& panel, const std::string& path)
	{
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host || host->IsHidden() || path.empty())
			return false;
		// 只登记整窗抓图请求:该窗口自己的帧循环会在"UI 提交之后、呈现之前"执行它。
		Renderer::RequestPresentCapture(host->GetPresentTarget(), path,
			static_cast<uint32_t>(host->ScreenRect().W), static_cast<uint32_t>(host->ScreenRect().H));
		return true;
	}

	bool EditorShell::AiRequestPreviewCapture(const std::string& panel, const std::string& path)
	{
		const auto found = m_PanelRegistry.find(panel);
		if (found == m_PanelRegistry.end() || path.empty())
			return false;
		if (auto* material = dynamic_cast<MaterialEditorPanel*>(found->second.get()))
		{
			material->RequestPreviewCapture(path);
			return true;
		}
		return false;
	}

	bool EditorShell::AiResizeWindow(const std::string& panel, float width, float height)
	{
		FloatWindowHost* host = FindFloatHost(panel);
		if (!host || host->IsHidden())
			return false;
		const uint32_t targetWidth = static_cast<uint32_t>(std::max(240.0f, width));
		const uint32_t targetHeight = static_cast<uint32_t>(std::max(160.0f, height));
		host->SetClientSize(targetWidth, targetHeight);
		m_LastFloatRects[panel] = Wui::WuiRect { host->ScreenRect().X, host->ScreenRect().Y,
			static_cast<float>(targetWidth), static_cast<float>(targetHeight) };
		return true;
	}

	std::string EditorShell::AiDescribeState() const
	{
		auto escape = [](const std::string& text)
		{
			std::string out;
			out.reserve(text.size() + 8);
			for (char c : text)
			{
				switch (c)
				{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default: out += c; break;
				}
			}
			return out;
		};
		std::ostringstream out;
		out << "{";
		out << "\"docked\":[";
		bool first = true;
		std::vector<Wui::PanelId> docked;
		m_Layout.AllPanels(&docked);
		for (const Wui::PanelId& id : docked)
		{
			if (!first)
				out << ",";
			first = false;
			out << "\"" << escape(id) << "\"";
		}
		out << "],\"floating\":[";
		first = true;
		for (const Wui::DockFloat& entry : m_Layout.Floating)
		{
			if (!first)
				out << ",";
			first = false;
			out << "{\"panel\":\"" << escape(entry.Panel) << "\",\"rect\":[" << entry.Rect.X << "," << entry.Rect.Y
				<< "," << entry.Rect.W << "," << entry.Rect.H << "]}";
		}
		out << "],\"independentWindows\":[";
		first = true;
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			if (host->IsHidden())
				continue;
			if (!first)
				out << ",";
			first = false;
			const Wui::WuiRect rect = host->ScreenRect();
			out << "{\"panel\":\"" << escape(host->Panel()) << "\",\"rect\":[" << rect.X << "," << rect.Y
				<< "," << rect.W << "," << rect.H << "],\"tabCount\":" << host->Panels().size() << "}";
		}
		out << "],\"materials\":[";
		first = true;
		for (const auto& entry : m_PanelRegistry)
		{
			const auto* material = dynamic_cast<const MaterialEditorPanel*>(entry.second.get());
			if (!material)
				continue;
			const Ref<Material>& asset = material->GetMaterial();
			if (!first)
				out << ",";
			first = false;
			out << "{\"panel\":\"" << escape(entry.first) << "\",\"path\":\"" << escape(material->GetMaterialPath()) << "\"";
			if (asset)
			{
				const MaterialDesc& desc = asset->GetDesc();
				out << ",\"albedo\":\"" << escape(desc.AlbedoTexture) << "\""
					<< ",\"normal\":\"" << escape(desc.NormalTexture) << "\""
					<< ",\"revision\":" << asset->GetRevision()
					<< ",\"baseColor\":[" << desc.BaseColor.r << "," << desc.BaseColor.g << ","
					<< desc.BaseColor.b << "," << desc.BaseColor.a << "]"
					<< ",\"blendMode\":" << static_cast<int>(desc.BlendMode);
			}
			out << "}";
		}
		// P3-1②:"附加/浮动态"是第一手断言目标(ui.open → ui.detach → ui.attach 的验收),
		// 这里按面板 id 列出形态(attached/floating/hidden),与 AttachTag 无障碍节点的
		// value 走同一个 PanelStateLabel —— 脚本不必去解析 floating/independentWindows 记录。
		std::vector<std::string> attachPanels = m_Panels;
		for (const auto& entry : m_PanelRegistry)
			if (std::find(attachPanels.begin(), attachPanels.end(), entry.first) == attachPanels.end())
				attachPanels.push_back(entry.first);
		for (const std::string& id : m_AttachedPanels)
			if (std::find(attachPanels.begin(), attachPanels.end(), id) == attachPanels.end())
				attachPanels.push_back(id);
		out << "],\"attach\":[";
		first = true;
		for (const std::string& id : attachPanels)
		{
			// 只有独立窗口形态的面板才有 attached/floating 两态;停靠面板的浮动是主窗口内
			// 临时浮动,不属于本清单(形态规则见 T03 / PanelStateLabel)。
			if (!IsDeclaredPanel(id) || !IsIndependentPanel(id))
				continue;
			if (!first)
				out << ",";
			first = false;
			out << "{\"panel\":\"" << escape(id) << "\",\"state\":\"" << PanelStateLabel(id) << "\"}";
		}
		out << "]}";
		return out.str();
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

	void EditorShell::OpenMaterialEditor(const std::string& path)
	{
		// 每个材质一个面板 + 独立窗口:同一材质重复打开只是前置焦点(用户 2026-09-16)。
		const std::string panelId = std::string(kMaterialPanelPrefix) + MaterialLibrary::NormalizePath(path);
		if (m_PanelRegistry.find(panelId) == m_PanelRegistry.end())
		{
			auto panel = std::make_unique<MaterialEditorPanel>(path);
			panel->OpenMaterial(path);
			m_PanelRegistry.emplace(panelId, std::move(panel));
			m_Panels.push_back(panelId);
			m_Layout.FloatMemory.push_back({ panelId, Wui::WuiRect { 200.0f, 170.0f, 760.0f, 470.0f } });
		}
		// D10:默认附加到主窗口(用户 2026-09-19);已拖成独立窗口的按原样只前置焦点。
		OpenPanelAttached(panelId);
	}

	void EditorShell::EnsureMaterialPanelFromId(const std::string& panelId)
	{
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kMaterialPanelPrefix), kMaterialPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kMaterialPanelPrefix));
		if (path == "(unsaved)")
			return;   // 未落盘的临时面板无法跨会话恢复
		auto panel = std::make_unique<MaterialEditorPanel>(path);
		panel->OpenMaterial(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
	}

	// ---- P1b D5:模型预览面板(动态实例,id = "model:<逻辑路径>")----

	void EditorShell::OpenModelPreview(const std::string& logicalPath)
	{
		// 每个模型一个面板 + 独立窗口:重复双击只是前置焦点(与材质编辑器同款;
		// 用户反馈"多次打开会多次叠加"—— 现在打开预览不改场景,放进场景在预览里显式点)。
		std::string path = logicalPath;
		std::replace(path.begin(), path.end(), '\\', '/');
		if (path.empty())
			return;
		const std::string panelId = std::string(kModelPanelPrefix) + path;
		if (m_PanelRegistry.find(panelId) == m_PanelRegistry.end())
		{
			auto panel = std::make_unique<ModelPreviewPanel>(path);
			m_PanelRegistry.emplace(panelId, std::move(panel));
			m_Panels.push_back(panelId);
			m_Layout.FloatMemory.push_back({ panelId, Wui::WuiRect { 220.0f, 160.0f, 620.0f, 660.0f } });
		}
		// D10:默认附加到主窗口(用户 2026-09-19);已拖成独立窗口的按原样只前置焦点。
		OpenPanelAttached(panelId);
	}

	void EditorShell::RequestImportDestination(const std::string& sourcePath)
	{
		// D10-10(用户 2026-09-19):导入位置选择器改成**窗口级模态**(此前是内容浏览器面板
		// 内的一层覆盖:位置偏、挡不住后面的输入)。范围仍限定内容根内 —— 原生文件夹对话框
		// 能选到工作区外,那种位置场景/打包都引用不到。状态与目录树由 shell 持有。
		if (sourcePath.empty())
			return;
		m_ImportSourcePath = std::filesystem::path(sourcePath);
		m_ImportStatus.clear();
		m_ImportTreeRoot = std::filesystem::path(WLD_ASSETPATH);
		m_ImportDestDir = m_ImportTreeRoot;
		const std::string panel = "content_browser";
		if (!m_Layout.Contains(panel))
			DockPanelBackToTree(panel);   // 内容浏览器被关掉时先让它回到停靠树(导入完能直接看到新文件)
		const auto found = m_PanelRegistry.find(panel);
		if (found != m_PanelRegistry.end())
			if (auto* browser = dynamic_cast<ContentBrowserPanel*>(found->second.get()))
				m_ImportDestDir = browser->CurrentDirectory();   // 默认落点 = 当前文件夹
		// 打开时扫一次内容根 + 重置模态自己的树状态(根行默认展开)。
		ScanImportTree();
		m_ImportTreeOpen.clear();
		// 默认落点(内容浏览器当前文件夹)可能在深层:把它的祖先链一起展开,
		// 打开时就能看到"选中"的那一行(只展开,不改选中)。
		for (std::filesystem::path dir = m_ImportDestDir; dir != m_ImportTreeRoot && dir.has_relative_path();)
		{
			m_ImportTreeOpen.insert(dir);
			const std::filesystem::path parent = dir.parent_path();
			if (parent == dir)
				break;   // 防御:到达盘符根仍不等于内容根时停止
			dir = parent;
		}
		m_ImportTreeOpen.insert(m_ImportTreeRoot);
		m_ImportTreeScroll = 0.0f;
		m_ImportModalOpen = true;
		if (m_Ctx)
		{
			// 打开前清掉悬着的弹窗;清文本焦点,否则后面面板里已聚焦的输入框还会继续吃键盘输入。
			m_Ctx->CloseAllPopups();
			m_Ctx->SetFocus(0);
			m_Ctx->RecordOp("import", "dest-open", m_ImportSourcePath.filename().string(), "");
		}
		WLD_CORE_INFO("[import] 选择导入位置(窗口级模态): {0}", sourcePath);
	}

	// D10-10:扫内容根下的全部子目录(低频操作:只在打开导入模态时跑一次)。
	// 与内容浏览器左侧树同一套数据形态:按路径排序 + 根行在最前(Depth 0)。
	void EditorShell::ScanImportTree()
	{
		m_ImportTree.clear();
		std::error_code scanError;
		std::filesystem::recursive_directory_iterator scanIt(m_ImportTreeRoot,
			std::filesystem::directory_options::skip_permission_denied, scanError);
		const std::filesystem::recursive_directory_iterator scanEnd;
		for (; scanIt != scanEnd; scanIt.increment(scanError))
		{
			if (scanError)
				break;   // 权限错误等:已扫到的部分照常可用(不抛异常、不中断整个选择器)
			const std::filesystem::directory_entry& entry = *scanIt;
			std::error_code entryError;
			if (!entry.is_directory(entryError))
				continue;
			ImportTreeRow row;
			row.Path = entry.path();
			row.Depth = scanIt.depth() + 1;
			std::error_code childError;
			for (const std::filesystem::directory_entry& child : std::filesystem::directory_iterator(row.Path,
				std::filesystem::directory_options::skip_permission_denied, childError))
			{
				if (childError)
					break;
				std::error_code childDirError;
				if (child.is_directory(childDirError))
				{
					row.HasChildren = true;
					break;
				}
			}
			m_ImportTree.push_back(std::move(row));
		}
		std::sort(m_ImportTree.begin(), m_ImportTree.end(),
			[](const ImportTreeRow& a, const ImportTreeRow& b) { return a.Path < b.Path; });
		ImportTreeRow rootRow;
		rootRow.Path = m_ImportTreeRoot;
		rootRow.Depth = 0;
		rootRow.HasChildren = !m_ImportTree.empty();
		m_ImportTree.insert(m_ImportTree.begin(), std::move(rootRow));
	}

	// D10-11:窗口级"选择导入位置"模态 —— 居中/遮罩/标题栏/Esc/按钮条走 WuiModal 组件;
	// "挡住后面所有面板的命中"由 OnRender 的 BeginModalInputBlock/EndModalInputBlock 成对负责
	// (见那里的顺序说明)。状态行仍归 shell(它有稳定的无障碍 id import.dest.status)。
	void EditorShell::RenderImportDestinationModal(Wui::WuiContext& ctx)
	{
		const Wui::WuiId modalId = Wui::HashId("modal.importdest");
		if (m_ImportModalOpen)
			ctx.SetModal(modalId);
		else if (ctx.Modal() == modalId)
			ctx.ClearModal();

		const auto logicalText = [this](const std::filesystem::path& dir)
		{
			const std::string relative = dir.lexically_relative(m_ImportTreeRoot).generic_string();
			return (relative.empty() || relative == ".") ? std::string("(内容根)") : relative;
		};

		Wui::WuiRect panel;
		bool escapePressed = false;
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = "选择导入位置";
		frameDesc.Size = { 560.0f, 440.0f };
		if (!Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			return;

		const float pad = 16.0f;
		Wui::Label(ctx, { panel.X + pad, panel.Y + 40.0f },
			"源文件: " + m_ImportSourcePath.filename().string()
				+ " · 导入到: " + logicalText(m_ImportDestDir),
			m_Theme.TextMuted, 13.0f);

		const float statusH = 20.0f;
		// 按钮条固定贴 frame 底部(ModalFooter 画),状态行排在它上方。
		const float statusY = panel.Y + panel.H - Wui::ModalFooterPadding - Wui::ModalFooterHeight - 6.0f - statusH;
		const Wui::WuiRect treeArea { panel.X + pad, panel.Y + 62.0f, panel.W - pad * 2.0f,
			std::max(40.0f, statusY - 8.0f - (panel.Y + 62.0f)) };
		Wui::PanelBackground(ctx, treeArea, { 0.09f, 0.095f, 0.10f, 1 });

		// 可见行:父行折叠 → 整棵子树不显示(m_ImportTree 是"父在子前"的预排序)。
		std::vector<const ImportTreeRow*> visibleRows;
		std::vector<Wui::TreeViewItem> treeItems;
		std::vector<Wui::WuiId> treeItemIds;
		std::vector<bool> openAtDepth;
		for (const ImportTreeRow& row : m_ImportTree)
		{
			if (row.Depth > 0)
			{
				if (row.Depth - 1 >= static_cast<int>(openAtDepth.size()))
					continue;   // 祖先行没显示 → 本行也不显示
				if (!openAtDepth[row.Depth - 1])
					continue;   // 直接父行折叠
			}
			if (static_cast<int>(openAtDepth.size()) > row.Depth)
				openAtDepth.resize(static_cast<size_t>(row.Depth));
			const bool expanded = m_ImportTreeOpen.find(row.Path) != m_ImportTreeOpen.end();
			openAtDepth.push_back(expanded);

			const std::filesystem::path rel = row.Path.lexically_relative(m_ImportTreeRoot);
			const std::string relText = (rel == ".") ? std::string() : rel.generic_string();
			Wui::TreeViewItem item;
			// 无障碍 id 约定(逐字,与 D10-9 一致):根行 = import.dest.tree.root,
			// 其它 = import.dest.tree.<相对路径>。
			item.Id = Wui::HashId(relText.empty() ? "import.dest.tree.root"
				: ("import.dest.tree." + relText).c_str());
			item.Label = row.Path.filename().string();   // 根行 = 内容根目录名
			item.Depth = row.Depth;
			item.HasChildren = row.HasChildren;
			item.Expanded = expanded;
			item.Selected = m_ImportDestDir == row.Path;
			visibleRows.push_back(&row);
			treeItemIds.push_back(item.Id);
			treeItems.push_back(std::move(item));
		}
		const Wui::TreeViewResult tree = Wui::TreeView(ctx, treeArea, treeItems, 20.0f, m_ImportTreeScroll, m_Theme);
		for (size_t i = 0; i < visibleRows.size(); ++i)
		{
			const ImportTreeRow& row = *visibleRows[i];
			if (tree.ClickedArrow == static_cast<int>(i))
			{
				if (treeItems[i].Expanded)
					m_ImportTreeOpen.erase(row.Path);
				else
					m_ImportTreeOpen.insert(row.Path);
			}
			else if (tree.Clicked == static_cast<int>(i))
			{
				m_ImportDestDir = row.Path;   // 单选:点行只改选中,不导航
			}
			// TreeView 自身不登记行节点(已知限制),按上面的 id 约定手动登记:
			// AI 可读可点;只登记落在树可视区内的行,避免点到看不见的行。
			if (i < tree.ItemRects.size() && treeItemIds[i] != 0)
			{
				const Wui::WuiRect& rowRect = tree.ItemRects[i];
				// 只登记"行中心确实落在树可视区内"的行:AI 注入的点击打在行中心,
				// 半滚出视口的行中心可能压到按钮行,点了会打错目标。
				const float rowCenterY = rowRect.Y + rowRect.H * 0.5f;
				if (rowRect.W > 0.0f && rowRect.H > 0.0f
					&& rowCenterY >= treeArea.Y && rowCenterY <= treeArea.Y + treeArea.H)
				{
					Wui::WuiAccessNode rowNode;
					rowNode.Id = treeItemIds[i];
					rowNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
					rowNode.Panel = "shell";
					rowNode.Kind = "tree-item";
					rowNode.Label = treeItems[i].Label;
					rowNode.Value = logicalText(row.Path);
					rowNode.Rect = rowRect;
					rowNode.Interactive = true;
					Wui::WuiAccessibility::Get().Register(rowNode);
				}
			}
		}

		bool closeRequested = false;
		const Wui::ModalResult footerResult = Wui::ModalFooter(ctx, panel, "导入到此文件夹", "取消",
			Wui::HashId("import.dest.ok"), Wui::HashId("import.dest.cancel"), true, m_Theme);
		if (footerResult == Wui::ModalResult::Confirm)
		{
			// 目的地 = 选中目录相对内容根的路径;**内容根本身传空串**(与内核/cook 约定一致)。
			const std::filesystem::path destRelative = m_ImportDestDir.lexically_relative(m_ImportTreeRoot);
			const std::string destRelativeText = destRelative.generic_string();
			const std::string destination =
				(destRelativeText.empty() || destRelativeText == ".") ? std::string() : destRelativeText;
			std::string message;
			std::string logicalModel;
			if (m_Editor.ImportModelFile(m_ImportSourcePath.string(), &message, &logicalModel, destination))
			{
				m_ImportStatus = message.empty() ? ("已导入到 " + logicalText(m_ImportDestDir)) : message;
				ctx.RecordOp("import", "dest-ok", m_ImportSourcePath.filename().string(), m_ImportStatus);
				WLD_CORE_INFO("[import] {0}", m_ImportStatus);
				// 内容浏览器刷新(它自己的公开入口)+ 模型预览。
				const auto browserFound = m_PanelRegistry.find("content_browser");
				if (browserFound != m_PanelRegistry.end())
					if (auto* browser = dynamic_cast<ContentBrowserPanel*>(browserFound->second.get()))
						browser->RefreshContents();
				if (!logicalModel.empty())
					OpenModelPreview(logicalModel);
				closeRequested = true;   // 成功后关闭;失败保持打开,用户可改选目录重试。
			}
			else
			{
				m_ImportStatus = message.empty() ? std::string("导入失败(宿主未给出原因)") : message;
				ctx.RecordOp("import", "dest-failed", m_ImportSourcePath.filename().string(), m_ImportStatus);
				WLD_CORE_WARN("[import] 导入 '{0}' 失败: {1}", m_ImportSourcePath.string(), m_ImportStatus);
			}
		}
		else if (footerResult == Wui::ModalResult::Cancel)
		{
			ctx.RecordOp("import", "dest-cancel", m_ImportSourcePath.filename().string(), "");
			closeRequested = true;
		}
		if (escapePressed)
			closeRequested = true;

		// 状态行:选中目录 + 上一次导入的可读结果(失败原因也写在这里)。
		const std::string statusText = "选中: " + logicalText(m_ImportDestDir)
			+ (m_ImportStatus.empty() ? std::string() : (" · " + m_ImportStatus));
		{
			Wui::WuiAccessNode statusNode;
			statusNode.Id = Wui::HashId("import.dest.status");
			statusNode.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			statusNode.Panel = "shell";
			statusNode.Kind = "status";
			statusNode.Label = "import destination status";
			statusNode.Value = statusText;
			statusNode.Rect = { panel.X + pad, statusY, panel.W - pad * 2.0f, statusH };
			statusNode.Interactive = false;
			Wui::WuiAccessibility::Get().Register(statusNode);
		}
		Wui::Label(ctx, { panel.X + pad, statusY }, statusText,
			m_ImportStatus.empty() ? m_Theme.TextMuted : m_Theme.Text, 13.0f);

		if (closeRequested)
		{
			m_ImportModalOpen = false;
			m_ImportStatus.clear();
			ctx.ClearModal();
		}
		Wui::EndModalFrame(ctx);
	}

	void EditorShell::EnsureModelPanelFromId(const std::string& panelId)
	{
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kModelPanelPrefix), kModelPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kModelPanelPrefix));
		if (path.empty())
			return;
		auto panel = std::make_unique<ModelPreviewPanel>(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
	}

	// ---- W9-2:脚本编辑器面板(动态实例,id = "script:<逻辑路径>")----

	void EditorShell::EnsureScriptPanelFromId(const std::string& panelId)
	{
		if (m_PanelRegistry.find(panelId) != m_PanelRegistry.end())
			return;
		if (panelId.compare(0, std::strlen(kScriptPanelPrefix), kScriptPanelPrefix) != 0)
			return;
		const std::string path = panelId.substr(std::strlen(kScriptPanelPrefix));
		if (path.empty())
			return;
		auto panel = std::make_unique<ScriptEditorPanel>(path);
		m_PanelRegistry.emplace(panelId, std::move(panel));
		if (std::find(m_Panels.begin(), m_Panels.end(), panelId) == m_Panels.end())
			m_Panels.push_back(panelId);
		// 默认窗口尺寸:代码编辑器需要足够宽高(自动 gutter + 状态行 + 工具栏都在默认客户区内)。
		if (m_LastFloatRects.find(panelId) == m_LastFloatRects.end())
			m_LastFloatRects[panelId] = Wui::WuiRect { 220.0f, 150.0f, 900.0f, 620.0f };
		if (!m_Layout.FindFloatMemory(panelId, nullptr))
			m_Layout.FloatMemory.push_back({ panelId, m_LastFloatRects[panelId] });
	}

	void EditorShell::OpenScriptEditor(const std::string& logicalPath)
	{
		// 面板渲染中途(内容浏览器双击 / Scripts 面板按钮)不能立刻"建新窗口(新 Vulkan
		// 交换链)+ 改附加标签":用户实测双击 .lua 会触发 vkQueueSubmit 设备丢失、界面黑屏。
		// 统一推迟到下一帧 OnRender 开头(与 AI 通道的帧首执行同为安全点)。
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty())
			return;
		if (std::find(m_PendingScriptOpen.begin(), m_PendingScriptOpen.end(), normalized) == m_PendingScriptOpen.end())
		{
			m_PendingScriptOpen.push_back(std::move(normalized));
			WLD_CORE_INFO("[script-editor] open request deferred to frame boundary");
		}
	}

	void EditorShell::OpenScriptEditorNow(const std::string& logicalPath)
	{
		// 用户决定(2026-09-18 C):打开即**默认直接附加到主窗口** —— OS 窗口保持隐藏,
		// 主窗口顶栏出现切换标签(点击在主界面/脚本内容之间切换);用户仍可把它拖出成独立窗口。
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty())
			return;
		const std::string panelId = std::string(kScriptPanelPrefix) + normalized;
		EnsureScriptPanelFromId(panelId);
		if (std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panelId) != m_AttachedPanels.end())
		{
			m_ActiveWindowTag = panelId; // 已打开:重复打开只是激活该标签
			return;
		}
		// 尚未附加:先按独立窗口建出(必要时复用已隐藏的窗口),随后立即挂靠到主窗口。
		if (!FindFloatHost(panelId))
			OpenIndependentPanel(panelId);
		AttachIndependentWindowToSlot(panelId);
	}

	void EditorShell::CloseEditorPanel(const std::string& panel)
	{
		if (!m_Ctx || panel.empty())
			return;
		// 与 Window 菜单/顶栏标签关闭同一条路径:已附加 → 摘标签并隐藏窗口;
		// 已浮动 → 隐藏复用;未打开 → 不做事(按钮只在面板可见时存在)。
		const auto attached = std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel);
		FloatWindowHost* host = FindFloatHost(panel);
		const bool visibleWindow = host && !host->IsHidden();
		if (attached == m_AttachedPanels.end() && !visibleWindow)
			return;
		TogglePanel(*m_Ctx, panel);
	}

	// W5-L1:文档场景外部改动提示(视口面板读;按钮触发重开,未保存时走确认模态)。
	bool EditorShell::ExternalSceneChanged() const
	{
		return m_Editor.ExternalSceneChanged();
	}

	void EditorShell::ReopenExternalScene()
	{
		m_Editor.ReopenExternalScene();
	}

	bool EditorShell::InstantiateModelFile(const std::string& logicalPath, std::string* message)
	{
		return m_Editor.InstantiateModelFile(logicalPath, message);
	}

	bool EditorShell::SaveProjectRenderSettings(const Asset::RenderingSettings& settings, std::string* message)
	{
		// 写盘口径与其它"回写清单"的地方一致:先 Load(保留 id/场景/包列表等字段),
		// 只覆盖 rendering,再 Validate + Save。
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		manifest.Rendering = settings;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		if (message)
			*message = "已保存渲染设置到 " + manifestPath.filename().string();
		return true;
	}

	bool EditorShell::SaveProjectPhysicsSettings(const Asset::PhysicsSettingsData& settings, std::string* message)
	{
		// 与渲染设置同一套"回写清单"口径:Load → 只覆盖 physics → Validate + Save。
		std::filesystem::path manifestPath;
		if (!Asset::ProjectManifest::Locate(std::filesystem::current_path(), &manifestPath))
		{
			if (message) *message = "找不到 project.we.yaml(工作目录下没有清单)";
			return false;
		}
		Asset::ProjectManifest manifest;
		std::string error;
		if (!Asset::ProjectManifest::Load(manifestPath, &manifest, &error))
		{
			if (message) *message = "清单读取失败: " + error;
			return false;
		}
		manifest.Physics = settings;
		if (!Asset::ProjectManifest::Save(manifestPath, manifest, &error))
		{
			if (message) *message = "清单写入失败: " + error;
			return false;
		}
		if (message)
			*message = "已保存物理设置到 " + manifestPath.filename().string();
		return true;
	}

	bool EditorShell::ImportModelFile(const std::string& sourcePath, std::string* message,
		std::string* outLogicalModel)
	{
		return m_Editor.ImportModelFile(sourcePath, message, outLogicalModel);
	}

	bool EditorShell::ImportModelFileTo(const std::string& sourcePath, const std::string& destinationLogicalDir,
		std::string* message, std::string* outLogicalModel)
	{
		return m_Editor.ImportModelFile(sourcePath, message, outLogicalModel, destinationLogicalDir);
	}

	EditorPanel* EditorShell::FocusedPanel()
	{
		// ① 主窗口处于"已附加面板"模式(顶栏标签):该面板就是焦点面板。
		if (!m_ActiveWindowTag.empty())
		{
			const auto found = m_PanelRegistry.find(m_ActiveWindowTag);
			if (found != m_PanelRegistry.end())
				return found->second.get();
		}
		// ② 独立窗口在前台:该窗口当前标签的面板。
		for (const std::unique_ptr<FloatWindowHost>& host : m_FloatHosts)
		{
			if (host->IsHidden() || !host->IsFocused())
				continue;
			const auto found = m_PanelRegistry.find(host->ActivePanel());
			if (found != m_PanelRegistry.end())
				return found->second.get();
		}
		// ③ 文本焦点所属面板(脚本编辑器 / 搜索框 / 重命名框等)。
		const std::string& textPanel = Wui::WuiTextFocus::Get().Panel();
		if (!textPanel.empty())
		{
			const auto found = m_PanelRegistry.find(textPanel);
			if (found != m_PanelRegistry.end())
				return found->second.get();
		}
		return nullptr;
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
		// 顶栏标签 = 一枚 OS 窗口(不是一枚面板):先把同一窗口里其它标签页的旧记录摘掉,
		// 再补上本次的面板 —— 否则会出现"一个窗口两枚 chip",拖出其中一枚会把整个窗口拔走。
		for (const std::string& id : panels)
			m_AttachedPanels.erase(std::remove(m_AttachedPanels.begin(), m_AttachedPanels.end(), id),
				m_AttachedPanels.end());
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

		// Header = 分组小标题行:只显示、不响应悬停/点击(菜单里区分"独立窗口/停靠面板")。
		struct MenuEntry { std::string Label; bool Checked; std::function<void()> Action; bool Header = false; };

		const Wui::WuiId menuFile = Wui::HashId("menu.file");
		const Wui::WuiId menuWindow = Wui::HashId("menu.window");
		if (!m_MenuBar)
		{
			m_MenuBar = std::make_shared<Wui::WuiBox>();
			m_MenuBar->Direction = Wui::WuiDirection::Row;
			m_MenuBar->Gap = 4;
			m_FileButton = std::make_shared<Wui::WuiButton>();
			m_FileButton->Label = Wui::Tr("menu.file", "File");
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
			m_WindowButton->Label = Wui::Tr("menu.window", "Window");
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
					if (entries[i].Header)
					{
						// 分组标题:淡色小字,不可点击。
						Label(ctx, { item.X + 4.0f, item.Y + 4.0f }, entries[i].Label, m_Theme.TextMuted, 12.0f);
						continue;
					}
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
			{ Wui::Tr("menu.file.new", "New"), false, [this] { m_Editor.NewScene(); } },
			{ Wui::Tr("menu.file.open", "Open"), false, [this] { m_Editor.OpenScene(); } },
			{ Wui::Tr("menu.file.save", "Save"), false, [this] { m_Editor.SaveScene(); } },
			{ Wui::Tr("menu.file.import", "Import glTF..."), false, [this] { m_Editor.ImportModelDialog(); } },
			{ Wui::Tr("menu.file.project_settings", "Project Settings"), false, [this]
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
			{ Wui::Tr("menu.file.editor_settings", "Editor Preferences"), false, [this, &ctx]
				{
					if (!FindFloatHost("prefs") && !m_Layout.Contains("prefs"))
						OpenIndependentPanel("prefs");
					TogglePanel(ctx, "prefs");
				} },
			{ Wui::Tr("menu.file.lua_stubs", "Generate Lua API Stubs"), false, [this] { m_Editor.GenerateLuaStubsAction(); } },
			{ Wui::Tr("menu.file.cooking", "Cooking"), false, [this] { m_Editor.StartCookingAction(); } },
			{ Wui::Tr("menu.file.export_ops", "Export Operation Log"), false, [this] { m_Editor.ExportOperationLog(); } },
			{ Wui::Tr("menu.file.exit", "Exit"), false, [this] { m_Editor.CloseAction(); } },
		});

		// 菜单分组:独立窗口(自带 OS 窗口)与停靠面板分开列,并给出不可点击的分组标题 ——
		// 之前两类平铺在一起,用户看不出"Gallery/Input Map 是独立窗口,其余是停靠标签"。
		std::vector<MenuEntry> windowEntries;
		const auto appendPanels = [this, &windowEntries, &ctx](bool independent)
		{
			for (const std::string& panel : m_Panels)
			{
				if (IsIndependentPanel(panel) != independent)
					continue;
				const bool visible = m_Layout.Contains(panel) || m_Layout.IsFloating(panel) ||
					std::find(m_AttachedPanels.begin(), m_AttachedPanels.end(), panel) != m_AttachedPanels.end();
				windowEntries.push_back({ PanelTitle(panel), visible, [this, panel, &ctx] { TogglePanel(ctx, panel); } });
			}
		};
		windowEntries.push_back({ Wui::Tr("menu.window.independent", "Independent Windows"), false, {}, true });
		appendPanels(/*independent=*/true);
		windowEntries.push_back({ Wui::Tr("menu.window.docked", "Docked Panels"), false, {}, true });
		appendPanels(/*independent=*/false);
		windowEntries.push_back({ Wui::Tr("menu.window.reset_layout", "Reset Layout"), false, [this, &ctx] { ResetLayout(ctx); } });
		drawMenu(menuWindow, "menu.window", windowEntries);
	}

	void EditorShell::DrawModals(Wui::WuiContext& ctx)
	{
		// ---- D10-10:导入位置(窗口级模态) ----
		// 放在这里(其余模态之前)是故意的:导入失败时 EditorLayer 会弹它自己的 Error 模态,
		// 那份错误框必须画在选择器**之上**才看得见(与 D10-9 面板内选择器时期的行为一致);
		// 选择器保持打开并把失败原因写进 import.dest.status。
		RenderImportDestinationModal(ctx);

		const Wui::WuiId unsaved = Wui::HashId("modal.unsaved");
		if (m_Editor.ShowUnsavedModal()) ctx.SetModal(unsaved);
		else if (ctx.Modal() == unsaved) ctx.ClearModal();
		{
			Wui::WuiRect panel;
			bool escapePressed = false;
			Wui::ModalFrameDesc frameDesc;
			frameDesc.Id = unsaved;
			frameDesc.Title = "Unsaved Changes";
			frameDesc.Size = { 460.0f, 150.0f };
			if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			{
				Label(ctx, { panel.X + 16, panel.Y + 48 }, "The current scene has unsaved changes.", m_Theme.Text, 14.0f);
				// 三按钮:Save / Don't Save / Cancel(Esc = Cancel),文案与顺序逐字保留。
				const Wui::ModalButtonDesc buttons[3] = {
					{ "Save", Wui::HashId("modal.unsaved.save"), true },
					{ "Don't Save", Wui::HashId("modal.unsaved.nosave"), true },
					{ "Cancel", Wui::HashId("modal.unsaved.cancel"), true },
				};
				const int clicked = Wui::ModalButtons(ctx, panel, buttons, 3, m_Theme);
				if (clicked == 0)
				{
					m_Editor.ResolveUnsavedModal(true);
					ctx.ClearModal();
				}
				else if (clicked == 1)
				{
					m_Editor.ResolveUnsavedModal(false);
					ctx.ClearModal();
				}
				else if (clicked == 2 || escapePressed)
				{
					m_Editor.CancelUnsavedModal();
					ctx.ClearModal();
				}
				Wui::EndModalFrame(ctx);
			}
		}

		const Wui::WuiId error = Wui::HashId("modal.error");
		if (m_Editor.ShowErrorModal()) ctx.SetModal(error);
		else if (ctx.Modal() == error) ctx.ClearModal();
		{
			Wui::WuiRect panel;
			bool escapePressed = false;
			Wui::ModalFrameDesc frameDesc;
			frameDesc.Id = error;
			frameDesc.Title = "Error";
			frameDesc.Size = { 460.0f, 150.0f };
			if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			{
				Label(ctx, { panel.X + 16, panel.Y + 48 }, m_Editor.ErrorText(), m_Theme.Text, 14.0f);
				const Wui::ModalButtonDesc buttons[1] = {
					{ "OK", Wui::HashId("modal.error.ok"), true },
				};
				const int clicked = Wui::ModalButtons(ctx, panel, buttons, 1, m_Theme);
				if (clicked == 0 || escapePressed)   // Esc = OK
				{
					m_Editor.ShowErrorModal() = false;
					m_Editor.ErrorText().clear();
					ctx.ClearModal();
				}
				Wui::EndModalFrame(ctx);
			}
		}

		const Wui::WuiId cooking = Wui::HashId("modal.cooking");
		if (m_Editor.ShowCookingProgress()) ctx.SetModal(cooking);
		else if (ctx.Modal() == cooking) ctx.ClearModal();
		{
			Wui::WuiRect panel;
			bool escapePressed = false;
			Wui::ModalFrameDesc frameDesc;
			frameDesc.Id = cooking;
			frameDesc.Title = "Packaging";
			frameDesc.Size = { 420.0f, 140.0f };
			if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			{
				if (m_Editor.CookingFinished())
				{
					const std::string message = m_Editor.CookingSucceeded() ? "Packaging finished." : "Packaging failed: " + m_Editor.CookingError();
					Label(ctx, { panel.X + 16, panel.Y + 48 }, message, m_Theme.Text, 14.0f);
					const Wui::ModalButtonDesc buttons[1] = {
						{ "OK", Wui::HashId("modal.cooking.ok"), true },
					};
					const int clicked = Wui::ModalButtons(ctx, panel, buttons, 1, m_Theme);
					if (clicked == 0 || escapePressed)   // 完成态才关(Esc = OK)
					{
						m_Editor.ShowCookingProgress() = false;
						ctx.ClearModal();
					}
				}
				else
				{
					Label(ctx, { panel.X + 16, panel.Y + 52 }, "Packaging...", m_Theme.Text, 14.0f);
					// 进度中不允许 Esc:不消费 escapePressed。
				}
				Wui::EndModalFrame(ctx);
			}
		}

		// ---- 项目设置 ----
		const Wui::WuiId projectSettings = Wui::HashId("modal.projectsettings");
		if (m_ShowProjectSettings)
		{
			ctx.SetModal(projectSettings);
			m_ShowProjectSettings = false;
		}
		{
			Wui::WuiRect panel;
			bool escapePressed = false;
			Wui::ModalFrameDesc frameDesc;
			frameDesc.Id = projectSettings;
			frameDesc.Title = "Project Settings";
			frameDesc.Size = { 380.0f, 190.0f };
			if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, m_Theme))
			{
				Label(ctx, { panel.X + 16, panel.Y + 48 }, "Renderer", m_Theme.TextMuted, 13.0f);
				std::vector<std::string> options = { "OpenGL", "Vulkan" };
				Combo(ctx, Wui::HashId("project.renderer"), { panel.X + 110, panel.Y + 46, 220, 24 },
					"", options, m_ProjectRendererIndex, m_Theme);
				const Wui::ModalButtonDesc buttons[2] = {
					{ "Save", Wui::HashId("project.save"), true },
					{ "Cancel", Wui::HashId("project.cancel"), true },
				};
				const int clicked = Wui::ModalButtons(ctx, panel, buttons, 2, m_Theme);
				if (clicked == 0)
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
				else if (clicked == 1 || escapePressed)   // Esc = Cancel
					ctx.ClearModal();
				Wui::EndModalFrame(ctx);
			}
		}
	}
}

