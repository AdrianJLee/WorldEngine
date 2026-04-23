#pragma once
#include "World.h"

#include <filesystem>
#include <set>
namespace World
{
	enum class ClipboardAction { None, Copy, Cut };

	class ContentBrowserPanel
	{
	public:
		ContentBrowserPanel();
		~ContentBrowserPanel() = default;

		void OnImGuiRender();

		// =========================================================
		// 动作注册拓展
		// =========================================================
		using OpenActionCallback = std::function<void(const std::filesystem::path&)>;
		void RegisterOpenAction(const std::string& extension, OpenActionCallback action);

	private:
		// =========================================================
		// 核心数据处理逻辑
		// =========================================================
		void OpenItem(const std::filesystem::path& path);
		void UpdateSearchCache();
		void CreateNewDirectory(const std::string& name);
		void PasteCopiedItems(const std::filesystem::path& destination);

		// =========================================================
		// UI 面板渲染模块
		// =========================================================
		void DrawNavigationBar();
		void DrawContentsGrid();
		void RenderItem(const std::filesystem::directory_entry& entry, bool showPath, const std::vector<std::filesystem::path>& currentDisplayPaths);

		// =========================================================
		// 弹窗与上下文相关层
		// =========================================================
		void DrawContextMenu();
		void DrawDeleteConfirmationModal();

	private:
		// 文件映射配置
		std::unordered_map<std::string, OpenActionCallback> m_OpenActions;

		// --- 路径跟踪状态 ---
		std::filesystem::path m_RootDirectory = WLD_ASSETPATH;
		std::filesystem::path m_CurrentDirectory;

		// --- 共用贴图资源 ---
		Ref<class Texture2D> m_DirectoryIcon;
		Ref<class Texture2D> m_FileIcon;

		// --- 检索状态缓存 ---
		char m_SearchBuffer[256] = { 0 };
		std::vector<std::filesystem::path> m_SearchResults;

		// --- 树级选则状态 ---
		std::set<std::filesystem::path> m_SelectedItems;
		std::filesystem::path m_LastSelectedItem;

		// --- 字符缓冲热键 ---
		std::filesystem::path m_ItemToRename;
		char m_RenameBuffer[256] = { 0 };

		// --- 操作预留弹层标记 ---
		bool m_ShowDeleteModal = false;

		// --- 剪贴数据通道 ---
		std::vector<std::filesystem::path> m_Clipboard;
		ClipboardAction m_ClipboardAction = ClipboardAction::None;
	};
}