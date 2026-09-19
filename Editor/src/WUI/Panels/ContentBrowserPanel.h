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
		// D10-10(用户 2026-09-19):导入位置选择器已搬到 EditorShell 的窗口级模态
		// (居中 + 全窗口挡输入),面板不再画自己的覆盖层;只保留 shell 需要的两个入口:
		// 导入成功后刷新列表(与工具栏 Refresh 同一条路径)。
		void RefreshContents();
		// 当前浏览目录:shell 打开导入模态时用它作默认落点(与双击导入/拖放同一约定)。
		const std::filesystem::path& CurrentDirectory() const { return m_Model.Current; }

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
		// D10-6:在指定目录下新建文件夹(树行右键菜单与工具栏/内容区菜单共用同一套命名/去重规则)。
		// 返回新建目录路径;创建失败返回空路径。
		std::filesystem::path CreateFolderIn(Wui::WuiContext& ctx, const std::filesystem::path& parentDir);
		// D3:新建材质资产(在当前目录写默认 .wmat 并在材质编辑器中打开)。
		void CreateMaterial(Wui::WuiContext& ctx);
		void ApplyRename(const std::filesystem::path& target, const std::string& newName);
		void StartRename(Wui::WuiContext& ctx, const std::filesystem::path& path);
		void RenderRenameField(Wui::WuiContext& ctx, const std::filesystem::path& path, const Wui::WuiRect& rect, const Wui::WuiTheme& theme);
		void OpenInExplorer(const std::filesystem::path& path);
		// D10-6:树行菜单「在资源管理器打开」——直接打开该目录本身(不是 /select 选中它)。
		void OpenFolderInExplorer(const std::filesystem::path& path);
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
		// D10-6:树行右键菜单的目标路径与钉住位置(跨帧保留,菜单关闭后清空)。
		std::filesystem::path m_TreeMenuPath;
		glm::vec2 m_TreeMenuPos {};
		// D10-6:从树行菜单发起的重命名目标;它的输入框画在树行上,内容区不再重复画。
		std::filesystem::path m_TreeRenameTarget;
		// D10:根文件夹行只自动展开一次(用户手动折叠后不再被强制展开)。
		bool m_RootRowSeeded = false;
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
