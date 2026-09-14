#pragma once

#include "EditorPanel.h"
#include "World/Renderer/Texture.h"
#include "World/WUI/WuiWidget.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace World
{
	// 目录树节点(缓存扫描结果,避免每帧全量扫盘)。
	struct BrowserDirNode
	{
		std::filesystem::path Path;
		int Depth = 0;
		bool HasChildren = false;
	};

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
		std::string RenameEdit;
		bool RenameActive = false;
		std::string SearchEdit;
		std::filesystem::path ContextMenuPath;
		glm::vec2 ContextMenuPos {};
		glm::vec2 BlankMenuPos {};
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
		std::vector<BrowserDirNode> DirTree;
		bool DirTreeDirty = true;
		std::filesystem::file_time_type TreeStamp {};
		std::chrono::steady_clock::time_point LastTreeCheck {};
		std::filesystem::path ListingPath;
		std::vector<std::filesystem::path> Listing;
		bool ListingDirty = true;
		std::filesystem::file_time_type ListingStamp {};
		std::chrono::steady_clock::time_point LastListingCheck {};
		std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, uintmax_t>> SizeCache;
	};

	class ContentBrowserPanel final : public EditorPanel
	{
	public:
		explicit ContentBrowserPanel(PanelHost& host);
		~ContentBrowserPanel();
		const char* Id() const override { return "content_browser"; }
		const char* Title() const override { return "Content Browser"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		void UpdateSearch();
		void OpenItem(const std::filesystem::path& path);
		void PasteInto(const std::filesystem::path& destination);
		void DeleteSelection();
		void Navigate(const std::filesystem::path& path);
		void Reveal(const std::filesystem::path& path);
		void GoBack();
		void GoUp();
		void CreateFolder(Wui::WuiContext& ctx);
		void ApplyRename(const std::filesystem::path& target, const std::string& newName);
		void StartRename(Wui::WuiContext& ctx, const std::filesystem::path& path);
		void RenderRenameField(Wui::WuiContext& ctx, const std::filesystem::path& path, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		void OpenInExplorer(const std::filesystem::path& path);
		void Cut();
		void Copy();
		void SelectAll(const std::vector<std::filesystem::path>& paths);
		void RefreshTree(bool force);
		void RefreshListing();
		uintmax_t FileSize(const std::filesystem::path& path);
		void InvalidateContents();
		void SaveState();
		void LoadState();

		PanelHost& m_Host;
		ContentBrowserModel m_Model;
		Ref<Texture2D> m_DirIcon;
		Ref<Texture2D> m_FileIcon;
		uint64_t m_DirIconId = 0;
		uint64_t m_FileIconId = 0;
		uint32_t m_IconGeneration = ~0u;
		uint32_t m_TextureEpoch = ~0u;
		std::shared_ptr<Wui::WuiBox> m_Toolbar;
		std::shared_ptr<Wui::WuiBox> m_Breadcrumbs;
		std::shared_ptr<Wui::WuiTextField> m_SearchField;
		std::shared_ptr<Wui::WuiButton> m_BackButton;
		std::shared_ptr<Wui::WuiButton> m_ForwardButton;
		std::shared_ptr<Wui::WuiButton> m_UpButton;
		std::shared_ptr<Wui::WuiButton> m_ViewModeButton;
		std::vector<std::shared_ptr<Wui::WuiButton>> m_CrumbButtons;
		std::vector<std::filesystem::path> m_CrumbDests;
		std::filesystem::path m_LastCrumbPath;
		std::filesystem::path m_StatePath;
		Wui::WuiContext* m_Ctx = nullptr;
	};
}
