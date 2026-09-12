#pragma once

#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiDock.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Scene/Components.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace World
{
	class EditorLayer;
	struct TypeSchema;
	struct FieldSchema;

	// 内容浏览器状态模型(与绘制分离)。
	struct ContentBrowserModel
	{
		std::filesystem::path Root = WLD_ASSETPATH;
		std::filesystem::path Current;
		char Search[256] = { 0 };
		std::vector<std::filesystem::path> SearchResults;
		std::set<std::filesystem::path> Selected;
		std::filesystem::path LastSelected;
		std::filesystem::path RenameTarget;
		char RenameBuffer[256] = { 0 };
		std::vector<std::filesystem::path> Clipboard;
		bool ClipboardCut = false;
		bool ShowDeleteModal = false;
		std::vector<std::filesystem::path> History;
		int HistoryIndex = -1;
		bool ShowNewFolderInput = false;
		char NewFolderBuffer[256] = { 0 };
		std::filesystem::path PendingDropDest;
		bool ListMode = false;
		std::set<std::filesystem::path> TreeOpen;
		float TreeScroll = 0;
		float ContentScroll = 0;
		Ref<Texture2D> DirIcon, FileIcon;
	};

	class EditorShell
	{
	public:
		explicit EditorShell(EditorLayer& editor);
		void OnRender(Wui::WuiContext& ctx);
		Wui::WuiRect ViewportRect() const { return m_ViewportRect; }

	private:
		// 停靠
		void RenderNode(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area);
		void RenderTabs(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area);
		void RenderSplit(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area);
		void RenderPanelContent(Wui::WuiContext& ctx, const std::string& id, const Wui::WuiRect& rect);
		void SaveLayout();

		// 面板
		void DrawMenuBar(Wui::WuiContext& ctx);
		void DrawHierarchy(Wui::WuiContext& ctx, const Wui::WuiRect& rect);
		void DrawProperties(Wui::WuiContext& ctx, const Wui::WuiRect& rect);
		void DrawContentBrowser(Wui::WuiContext& ctx, const Wui::WuiRect& rect);
		void DrawViewport(Wui::WuiContext& ctx, const Wui::WuiRect& rect);
		void DrawStats(Wui::WuiContext& ctx, const Wui::WuiRect& rect);
		void DrawMemory(Wui::WuiContext& ctx, const Wui::WuiRect& rect);
		void DrawOperations(Wui::WuiContext& ctx, const Wui::WuiRect& rect);
		void DrawModals(Wui::WuiContext& ctx);
		void RestoreLayout(const std::string& json);
		void RecordDockChange(Wui::WuiContext& ctx, const std::string& action, const std::string& target, const std::string& before);
		void TogglePanel(Wui::WuiContext& ctx, const std::string& panel);
		void ResetLayout(Wui::WuiContext& ctx);

		// Inspector 辅助
		float DrawSchemaFields(Wui::WuiContext& ctx, Wui::WuiId base, const Wui::WuiRect& rect, void* instance,
			const std::string& typeName, const Schema::TypeSchema& schema);
		float DrawComponentInspector(Wui::WuiContext& ctx, const Wui::WuiRect& rect, Entity entity, const Schema::TypeSchema& schema);

		// 内容浏览器
		void UpdateBrowserSearch();
		void BrowserOpenItem(const std::filesystem::path& path);
		void BrowserPasteInto(const std::filesystem::path& destination);
		void BrowserDeleteSelection();
		void BrowserNavigate(const std::filesystem::path& path);
		void BrowserReveal(const std::filesystem::path& path);
		void BrowserGoBack();
		void BrowserGoUp();
		void BrowserCreateFolder();
		void BrowserApplyRename(const std::filesystem::path& target, const std::string& newName);
		void BrowserCut();
		void BrowserCopy();
		void BrowserSelectAll(const std::vector<std::filesystem::path>& paths);

		EditorLayer& m_Editor;
		Wui::DockLayout m_Layout;
		std::filesystem::path m_LayoutPath;
		std::vector<std::string> m_Panels;
		Wui::WuiTheme m_Theme;
		ContentBrowserModel m_Browser;
		Wui::WuiRect m_ViewportRect;

		bool m_SplitterDragging = false;
		Wui::DockNode* m_DragSplitNode = nullptr;
		bool m_DragSplitRow = true;
		std::string m_SplitterBeforeJson;
		bool m_GizmoActive = false;
		TransformComponent m_GizmoBefore;
		std::string m_DropTargetPanel;
		Wui::DropZone m_DropZone = Wui::DropZone::Center;
		std::string m_LastDragTarget;
		Wui::DropZone m_LastDragZone = Wui::DropZone::Center;
		Wui::WuiContext* m_Ctx = nullptr;
	};
}
