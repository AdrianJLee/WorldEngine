#pragma once

#include "Panels/ContentBrowserPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/PropertiesPanel.h"
#include "Panels/ReadoutPanels.h"
#include "Panels/ViewportPanel.h"

#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiDock.h"
#include "World/WUI/WuiWidgets.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	class EditorLayer;

	// 编辑器外壳:停靠布局、菜单栏、模态与面板注册表。业务面板已组件化到
	// Panels/ 目录,外壳只负责驱动它们渲染并通过 PanelHost / ViewportHost 供给能力。
	class EditorShell : public ViewportHost
	{
	public:
		explicit EditorShell(EditorLayer& editor);
		~EditorShell();
		void OnRender(Wui::WuiContext& ctx);
		Wui::WuiRect ViewportRect() const { return m_ViewportRect; }

		// ---- PanelHost ----
		Ref<Scene> GetActiveScene() override;
		Entity GetSelectedEntity() override;
		void SetSelectedEntity(Entity entity) override;
		void MarkDocumentDirty() override;
		void DuplicateSelectedEntity() override;
		void OpenScene(const std::filesystem::path& path) override;
		Wui::WuiTheme& Theme() override { return m_Theme; }

		// ---- ViewportHost ----
		bool HasRenderedScene() const override;
		Ref<SceneRenderer>& GetSceneRenderer() override;
		void SetViewportState(bool focused, bool hovered, glm::vec2 size, glm::vec2 bounds[2]) override;
		void SetViewportRect(const Wui::WuiRect& rect) override;
		bool IsPlaying() const override;
		bool IsSimulating() const override;
		bool IsPaused() const override;
		void TogglePlay() override;
		void ToggleSimulate() override;
		void TogglePause() override;
		Ref<Texture2D> GetIcon(int index) const override;
		uint64_t GetIconId(int index) const override;
		uint64_t GetSceneTextureId() const override;
		Entity PickEntityAt(glm::vec2 viewportLocal) override;
		EditorCamera& GetEditorCamera() override;
		Wui::GizmoOperation GetGizmoOperation() const override;

	private:
		// 停靠
		void RenderNode(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area);
		void RenderTabs(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area);
		void RenderSplit(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area);
		void RenderPanelContent(Wui::WuiContext& ctx, const std::string& id, const Wui::WuiRect& rect);
		void SaveLayout();

		// 菜单 / 模态 / 布局
		void DrawMenuBar(Wui::WuiContext& ctx);
		void DrawModals(Wui::WuiContext& ctx);
		void RestoreLayout(const std::string& json);
		void RecordDockChange(Wui::WuiContext& ctx, const std::string& action, const std::string& target, const std::string& before);
		void TogglePanel(Wui::WuiContext& ctx, const std::string& panel);
		void ResetLayout(Wui::WuiContext& ctx);
		const char* PanelTitle(const std::string& id) const;

		EditorLayer& m_Editor;
		Wui::DockLayout m_Layout;
		std::filesystem::path m_LayoutPath;
		std::vector<std::string> m_Panels;
		Wui::WuiTheme m_Theme;
		std::unordered_map<std::string, std::unique_ptr<EditorPanel>> m_PanelRegistry;
		Wui::WuiRect m_ViewportRect;

		bool m_SplitterDragging = false;
		bool m_ShowProjectSettings = false;
		int m_ProjectRendererIndex = 0;
		Wui::DockNode* m_DragSplitNode = nullptr;
		bool m_DragSplitRow = true;
		std::string m_SplitterBeforeJson;
		std::string m_DropTargetPanel;
		Wui::DropZone m_DropZone = Wui::DropZone::Center;
		std::string m_LastDragTarget;
		Wui::DropZone m_LastDragZone = Wui::DropZone::Center;
	};
}
