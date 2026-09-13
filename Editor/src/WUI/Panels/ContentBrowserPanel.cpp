#include "wldpch.h"
#include "ContentBrowserPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Renderer/Texture.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace World
{
	namespace
	{
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

		// 判断 candidate 是否等于 root 或位于 root 的子树内。
		bool IsWithinOrEqual(const std::filesystem::path& candidate, const std::filesystem::path& root)
		{
			if (candidate == root)
				return true;
			const std::filesystem::path relative = candidate.lexically_relative(root);
			const std::string text = relative.generic_string();
			return !text.empty() && text.rfind("..", 0) != 0;
		}
	}

	ContentBrowserPanel::ContentBrowserPanel(PanelHost& host)
		: m_Host(host), m_StatePath(std::string(WLD_EDITOR_DIR) + "wui-browser.json")
	{
		m_Model.Current = m_Model.Root;
		LoadState();
	}

	ContentBrowserPanel::~ContentBrowserPanel()
	{
		SaveState();
	}

	void ContentBrowserPanel::UpdateSearch()
	{
		m_Model.SearchResults.clear();
		std::string query = m_Model.Search;
		std::transform(query.begin(), query.end(), query.begin(), ::tolower);
		std::error_code searchError;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(m_Model.Current, std::filesystem::directory_options::skip_permission_denied, searchError))
		{
			if (searchError)
				break;
			std::string name = entry.path().filename().string();
			std::transform(name.begin(), name.end(), name.begin(), ::tolower);
			if (name.find(query) != std::string::npos)
				m_Model.SearchResults.push_back(entry.path());
		}
	}

	void ContentBrowserPanel::RefreshTree(bool force)
	{
		const auto now = std::chrono::steady_clock::now();
		if (!force && !m_Model.DirTreeDirty && std::chrono::duration<double>(now - m_Model.LastTreeCheck).count() < 0.5)
			return;
		m_Model.LastTreeCheck = now;

		std::error_code stampError;
		const auto stamp = std::filesystem::last_write_time(m_Model.Root, stampError);
		if (!force && !m_Model.DirTreeDirty && !stampError && stamp == m_Model.TreeStamp)
			return;

		m_Model.DirTree.clear();
		std::error_code scanError;
		std::filesystem::recursive_directory_iterator scanIt(m_Model.Root, std::filesystem::directory_options::skip_permission_denied, scanError);
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
			m_Model.DirTree.push_back(std::move(node));
		}
		std::sort(m_Model.DirTree.begin(), m_Model.DirTree.end(), [](const BrowserDirNode& a, const BrowserDirNode& b) { return a.Path < b.Path; });
		m_Model.TreeStamp = stamp;
		m_Model.DirTreeDirty = false;
	}

	void ContentBrowserPanel::RefreshListing()
	{
		const auto now = std::chrono::steady_clock::now();
		if (!m_Model.ListingDirty && m_Model.ListingPath == m_Model.Current && std::chrono::duration<double>(now - m_Model.LastListingCheck).count() < 0.5)
			return;
		m_Model.LastListingCheck = now;

		std::error_code stampError;
		const auto stamp = std::filesystem::last_write_time(m_Model.Current, stampError);
		if (!m_Model.ListingDirty && m_Model.ListingPath == m_Model.Current && !stampError && stamp == m_Model.ListingStamp)
			return;

		m_Model.Listing.clear();
		std::error_code listError;
		for (const auto& entry : std::filesystem::directory_iterator(m_Model.Current, std::filesystem::directory_options::skip_permission_denied, listError))
		{
			if (listError)
				break;
			m_Model.Listing.push_back(entry.path());
		}
		std::sort(m_Model.Listing.begin(), m_Model.Listing.end());
		m_Model.ListingPath = m_Model.Current;
		m_Model.ListingStamp = stamp;
		m_Model.ListingDirty = false;
	}

	uintmax_t ContentBrowserPanel::FileSize(const std::filesystem::path& path)
	{
		std::error_code stampError;
		const auto stamp = std::filesystem::last_write_time(path, stampError);
		const auto cached = m_Model.SizeCache.find(path);
		if (!stampError && cached != m_Model.SizeCache.end() && cached->second.first == stamp)
			return cached->second.second;
		std::error_code sizeError;
		const uintmax_t size = std::filesystem::file_size(path, sizeError);
		const uintmax_t result = sizeError ? 0 : size;
		m_Model.SizeCache[path] = { stamp, result };
		return result;
	}

	void ContentBrowserPanel::InvalidateContents()
	{
		m_Model.DirTreeDirty = true;
		m_Model.ListingDirty = true;
		m_Model.TreeStamp = {};
		m_Model.ListingStamp = {};
	}

	void ContentBrowserPanel::SaveState()
	{
		try
		{
			Wui::JsonValue root;
			root.type = Wui::JsonValue::Type::Object;
			root.Object.push_back({ "listMode", Wui::JsonValue::MakeBool(m_Model.ListMode) });
			root.Object.push_back({ "current", Wui::JsonValue::MakeString(m_Model.Current.lexically_relative(m_Model.Root).generic_string()) });
			root.Object.push_back({ "treeScroll", Wui::JsonValue::MakeNumber(m_Model.TreeScroll) });
			root.Object.push_back({ "contentScroll", Wui::JsonValue::MakeNumber(m_Model.ContentScroll) });
			Wui::JsonValue open;
			open.type = Wui::JsonValue::Type::Array;
			for (const auto& path : m_Model.TreeOpen)
				open.Array.push_back(Wui::JsonValue::MakeString(path.lexically_relative(m_Model.Root).generic_string()));
			root.Object.push_back({ "treeOpen", std::move(open) });
			std::ofstream stream(m_StatePath, std::ios::binary | std::ios::trunc);
			if (stream)
				stream << root.Dump();
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Failed to save content browser state: {0}", error.what());
		}
	}

	void ContentBrowserPanel::LoadState()
	{
		if (!std::filesystem::exists(m_StatePath))
			return;
		try
		{
			std::ifstream stream(m_StatePath, std::ios::binary);
			if (!stream)
				return;
			std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			std::string error;
			const auto parsed = Wui::JsonValue::Parse(text, &error);
			if (!parsed)
				return;
			if (const Wui::JsonValue* value = parsed->Find("listMode"))
				m_Model.ListMode = value->AsBool(false);
			if (const Wui::JsonValue* value = parsed->Find("current"))
			{
				const std::string relative = value->AsString("");
				if (!relative.empty())
				{
					const std::filesystem::path target = m_Model.Root / std::filesystem::path(relative);
					std::error_code dirError;
					if (std::filesystem::is_directory(target, dirError))
						m_Model.Current = target;
				}
			}
			if (const Wui::JsonValue* value = parsed->Find("treeScroll"))
				m_Model.TreeScroll = static_cast<float>(value->AsNumber(0));
			if (const Wui::JsonValue* value = parsed->Find("contentScroll"))
				m_Model.ContentScroll = static_cast<float>(value->AsNumber(0));
			if (const Wui::JsonValue* value = parsed->Find("treeOpen"))
			{
				m_Model.TreeOpen.clear();
				for (const auto& item : value->Array)
				{
					const std::string relative = item.AsString("");
					if (!relative.empty())
						m_Model.TreeOpen.insert(m_Model.Root / std::filesystem::path(relative));
				}
			}
		}
		catch (const std::exception& error)
		{
			WLD_CORE_WARN("Failed to load content browser state: {0}", error.what());
		}
	}

	void ContentBrowserPanel::Navigate(const std::filesystem::path& path)
	{
		if (m_Model.HistoryIndex >= 0 && m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) && m_Model.History[m_Model.HistoryIndex] == path)
			return;
		if (m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) - 1)
			m_Model.History.resize(m_Model.HistoryIndex + 1);
		m_Model.History.push_back(path);
		m_Model.HistoryIndex = static_cast<int>(m_Model.History.size()) - 1;
		m_Model.Current = path;
		m_Model.Selected.clear();
		m_Model.LastSelected.clear();
		Reveal(path);
		m_Model.ListingDirty = true;
		m_Model.ListingStamp = {};
		SaveState();
		if (m_Model.Search[0])
			UpdateSearch();
	}

	void ContentBrowserPanel::Reveal(const std::filesystem::path& path)
	{
		std::filesystem::path current = path;
		while (current != m_Model.Root && !current.empty() && current.has_parent_path())
		{
			m_Model.TreeOpen.insert(current);
			const std::filesystem::path parent = current.parent_path();
			if (parent == current)
				break;
			current = parent;
		}
	}

	void ContentBrowserPanel::GoBack()
	{
		if (m_Model.HistoryIndex > 0)
		{
			--m_Model.HistoryIndex;
			m_Model.Current = m_Model.History[m_Model.HistoryIndex];
			m_Model.Selected.clear();
			m_Model.LastSelected.clear();
			Reveal(m_Model.Current);
			m_Model.ListingDirty = true;
			m_Model.ListingStamp = {};
			SaveState();
			if (m_Model.Search[0])
				UpdateSearch();
		}
	}

	void ContentBrowserPanel::GoUp()
	{
		if (m_Model.Current != m_Model.Root)
			Navigate(m_Model.Current.parent_path());
	}

	void ContentBrowserPanel::OpenItem(const std::filesystem::path& path)
	{
		if (std::filesystem::is_directory(path))
		{
			Navigate(path);
			return;
		}
		if (path.extension() == ".wd")
			m_Host.OpenScene(path);
		else
		{
			const std::string cmd = "start \"\" \"" + std::filesystem::absolute(path).string() + "\"";
			system(cmd.c_str());
		}
	}

	void ContentBrowserPanel::PasteInto(const std::filesystem::path& destination)
	{
		for (const auto& path : m_Model.Clipboard)
		{
			if (!std::filesystem::exists(path))
				continue;
			std::filesystem::path destPath = destination / path.filename();
			if (m_Model.ClipboardCut)
			{
				if (path != destPath)
				{
					std::filesystem::rename(path, destPath);
					if (m_Ctx) m_Ctx->RecordOp("browser", "move", path.filename().string(), "-> " + destination.string());
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
				if (m_Ctx) m_Ctx->RecordOp("browser", "copy", path.filename().string(), "-> " + destPath.string());
			}
		}
		if (m_Model.ClipboardCut)
		{
			m_Model.Clipboard.clear();
			m_Model.ClipboardCut = false;
		}
		InvalidateContents();
		SaveState();
		if (m_Model.Search[0])
			UpdateSearch();
	}

	void ContentBrowserPanel::DeleteSelection()
	{
		// 先快照目标:父目录删除时已覆盖其子项,跳过冗余 remove_all。
		const std::vector<std::filesystem::path> targets(m_Model.Selected.begin(), m_Model.Selected.end());
		const size_t count = targets.size();
		bool removedCurrent = false;
		for (const auto& path : targets)
		{
			if (m_Model.Current == path || IsWithinOrEqual(m_Model.Current, path))
				removedCurrent = true;
			const bool covered = std::any_of(targets.begin(), targets.end(),
				[&](const std::filesystem::path& other) { return other != path && IsWithinOrEqual(path, other); });
			if (covered)
				continue;
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
			if (ec)
				WLD_CORE_WARN("Content browser delete failed for '{0}': {1}", path.string(), ec.message());
		}

		// 清理指向已删除路径的模型状态,避免残留导航、展开与拖放目标。
		const auto referenced = [&](const std::filesystem::path& entry)
		{
			return std::any_of(targets.begin(), targets.end(),
				[&](const std::filesystem::path& target) { return IsWithinOrEqual(entry, target); });
		};
		for (auto it = m_Model.TreeOpen.begin(); it != m_Model.TreeOpen.end();)
		{
			if (referenced(*it)) it = m_Model.TreeOpen.erase(it);
			else ++it;
		}
		m_Model.History.erase(std::remove_if(m_Model.History.begin(), m_Model.History.end(), referenced), m_Model.History.end());
		m_Model.HistoryIndex = static_cast<int>(m_Model.History.size()) - 1;
		m_Model.Clipboard.erase(std::remove_if(m_Model.Clipboard.begin(), m_Model.Clipboard.end(), referenced), m_Model.Clipboard.end());
		if (!m_Model.RenameTarget.empty() && referenced(m_Model.RenameTarget))
		{
			m_Model.RenameTarget.clear();
			m_Model.RenameEdit.clear();
			m_Model.RenameActive = false;
		}
		if (!m_Model.PendingDropDest.empty() && referenced(m_Model.PendingDropDest))
			m_Model.PendingDropDest.clear();

		m_Model.Selected.clear();
		m_Model.LastSelected.clear();
		if (removedCurrent)
			Navigate(m_Model.Root);
		InvalidateContents();
		SaveState();
		if (m_Model.Search[0])
			UpdateSearch();
		if (m_Ctx) m_Ctx->RecordOp("browser", "delete", std::to_string(count), "");
	}

	void ContentBrowserPanel::CreateFolder(Wui::WuiContext& ctx)
	{
		std::filesystem::path newPath = m_Model.Current / "New Folder";
		int counter = 1;
		while (std::filesystem::exists(newPath))
			newPath = m_Model.Current / ("New Folder (" + std::to_string(counter++) + ")");
		try
		{
			std::filesystem::create_directory(newPath);
			m_Model.Selected.clear();
			m_Model.Selected.insert(newPath);
			m_Model.LastSelected = newPath;
			InvalidateContents();
			SaveState();
			if (m_Ctx) m_Ctx->RecordOp("browser", "mkdir", newPath.filename().string(), "");
			StartRename(ctx, newPath);
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("Could not create directory: {0}", error.what());
		}
	}

	void ContentBrowserPanel::ApplyRename(const std::filesystem::path& target, const std::string& newName)
	{
		if (newName.empty())
		{
			m_Model.RenameTarget.clear();
			m_Model.RenameActive = false;
			return;
		}
		const std::filesystem::path newPath = target.parent_path() / newName;
		if (newPath != target)
		{
			std::error_code ignored;
			std::filesystem::rename(target, newPath, ignored);
			if (m_Ctx) m_Ctx->RecordOp("browser", "rename", target.filename().string(), "-> " + newPath.filename().string());
		}
		m_Model.RenameTarget.clear();
		m_Model.RenameActive = false;
		InvalidateContents();
		SaveState();
		if (m_Model.Search[0])
			UpdateSearch();
	}

	void ContentBrowserPanel::StartRename(Wui::WuiContext& ctx, const std::filesystem::path& path)
	{
		m_Model.RenameTarget = path;
		m_Model.RenameEdit = path.filename().string();
		std::strncpy(m_Model.RenameBuffer, m_Model.RenameEdit.c_str(), sizeof(m_Model.RenameBuffer) - 1);
		m_Model.RenameActive = true;
		ctx.SetFocus(Wui::HashId("browser.rename"));
		ctx.SetTextInputActive(true);
	}

	void ContentBrowserPanel::RenderRenameField(Wui::WuiContext& ctx, const std::filesystem::path& path, const Wui::WuiRect& rect, const Wui::WuiTheme& theme)
	{
		const Wui::WuiId renameId = Wui::HashId("browser.rename");
		bool cancelled = false;
		if (TextField(ctx, renameId, rect, m_Model.RenameEdit, theme, &cancelled))
		{
			// 回车提交
			ApplyRename(path, m_Model.RenameEdit);
		}
		else if (cancelled)
		{
			// Escape 丢弃
			m_Model.RenameTarget.clear();
			m_Model.RenameEdit.clear();
			m_Model.RenameActive = false;
		}
		else if (m_Model.RenameActive && ctx.Focus() != renameId)
		{
			// 失焦提交:点选其他条目或空白处时收起重命名框。
			ApplyRename(path, m_Model.RenameEdit);
		}
	}

	void ContentBrowserPanel::OpenInExplorer(const std::filesystem::path& path)
	{
		// /select 让资源管理器打开所在文件夹并选中该项,而不是直接打开文件。
		const std::wstring parameters = L"/select,\"" + std::filesystem::absolute(path).wstring() + L"\"";
		const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<intptr_t>(result) <= 32)
			WLD_CORE_WARN("Could not open Explorer for '{0}' (error {1})", path.string(), reinterpret_cast<intptr_t>(result));
	}

	void ContentBrowserPanel::Cut()
	{
		m_Model.Clipboard.assign(m_Model.Selected.begin(), m_Model.Selected.end());
		m_Model.ClipboardCut = true;
	}

	void ContentBrowserPanel::Copy()
	{
		m_Model.Clipboard.assign(m_Model.Selected.begin(), m_Model.Selected.end());
		m_Model.ClipboardCut = false;
	}

	void ContentBrowserPanel::SelectAll(const std::vector<std::filesystem::path>& paths)
	{
		m_Model.Selected.clear();
		m_Model.Selected.insert(paths.begin(), paths.end());
		if (!paths.empty())
			m_Model.LastSelected = paths.back();
	}

	void ContentBrowserPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		m_Ctx = &ctx;
		const Wui::WuiTheme& theme = host.Theme();
		if (!m_Model.DirIcon)
			m_Model.DirIcon = Texture2D::Create("Resource/Icons/ContentBrowser/DirectoryIcon.png");
		if (!m_Model.FileIcon)
			m_Model.FileIcon = Texture2D::Create("Resource/Icons/ContentBrowser/FileIcon.png");

		std::string filePayload;
		const bool fileDrag = ctx.IsDragActive(&filePayload) && filePayload.rfind("file:", 0) == 0;

		float y = rect.Y + 6;
		const bool canBack = m_Model.HistoryIndex > 0;
		const bool canForward = m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) - 1;
		if (Button(ctx, Wui::HashId("browser.back"), { rect.X + 6, y, 24, 24 }, "<", theme) && canBack)
			GoBack();
		if (Button(ctx, Wui::HashId("browser.forward"), { rect.X + 32, y, 24, 24 }, ">", theme) && canForward)
		{
			++m_Model.HistoryIndex;
			m_Model.Current = m_Model.History[m_Model.HistoryIndex];
			m_Model.Selected.clear();
			m_Model.LastSelected.clear();
			Reveal(m_Model.Current);
			m_Model.ListingDirty = true;
			m_Model.ListingStamp = {};
			SaveState();
			if (m_Model.Search[0])
				UpdateSearch();
		}
		if (Button(ctx, Wui::HashId("browser.up"), { rect.X + 58, y, 32, 24 }, "Up", theme) && m_Model.Current != m_Model.Root)
			GoUp();

		float x = rect.X + 96;
		auto breadcrumb = [&](const std::string& label, const std::filesystem::path& destination, const Wui::WuiId id)
		{
			const Wui::WuiRect btn { x, y, static_cast<float>(label.size() * 8 + 20), 24 };
			if (fileDrag && ctx.IsHovered(btn))
			{
				ctx.DropTarget(btn, "file:");
				m_Model.PendingDropDest = destination;
				ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, btn, theme.Accent, 2.0f, 2.0f });
			}
			if (Button(ctx, id, btn, label, theme))
				Navigate(destination);
			x += btn.W + 6;
		};
		breadcrumb("Root", m_Model.Root, Wui::HashId("browser.crumb.root"));
		std::filesystem::path accumulated = m_Model.Root;
		for (const auto& part : m_Model.Current.lexically_relative(m_Model.Root))
		{
			accumulated /= part;
			breadcrumb(part.string(), accumulated, Wui::HashId(("browser.crumb." + part.string()).c_str()));
		}

		if (Button(ctx, Wui::HashId("browser.viewmode"), { rect.X + rect.W - 372, y, 58, 24 }, m_Model.ListMode ? "Grid" : "List", theme))
		{
			m_Model.ListMode = !m_Model.ListMode;
			SaveState();
		}
		if (Button(ctx, Wui::HashId("browser.newfolder"), { rect.X + rect.W - 308, y, 70, 24 }, "+ Folder", theme))
			CreateFolder(ctx);
		if (Button(ctx, Wui::HashId("browser.refresh"), { rect.X + rect.W - 232, y, 58, 24 }, "Refresh", theme))
		{
			InvalidateContents();
			if (m_Model.Search[0])
				UpdateSearch();
		}
		{
			if (m_Model.SearchEdit.empty() && m_Model.Search[0])
				m_Model.SearchEdit = m_Model.Search;
			if (TextField(ctx, Wui::HashId("browser.search"), { rect.X + rect.W - 168, y, 162, 24 }, m_Model.SearchEdit, theme))
			{
				std::strncpy(m_Model.Search, m_Model.SearchEdit.c_str(), sizeof(m_Model.Search) - 1);
				m_Model.Search[sizeof(m_Model.Search) - 1] = 0;
				UpdateSearch();
			}
		}
		y += 32;

		// ---- 左侧目录树 ----
		const float treeW = 190;
		const Wui::WuiRect treeRect { rect.X, y, treeW, rect.H - (y - rect.Y) };
		ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, treeRect, { 0.09f, 0.095f, 0.10f, 1 }, 0.0f });
		RefreshTree(false);
		BeginScrollArea(ctx, treeRect, m_Model.DirTree.size() * 20.0f + 8, m_Model.TreeScroll, theme);
		float ty = treeRect.Y + 4 - m_Model.TreeScroll;
		for (const BrowserDirNode& node : m_Model.DirTree)
		{
			if (node.Depth > 1 && m_Model.TreeOpen.find(node.Path.parent_path()) == m_Model.TreeOpen.end())
				continue;
			const Wui::WuiRect row { treeRect.X + 4 + node.Depth * 12, ty, treeW - 8 - node.Depth * 12, 20 };
			const bool isCurrent = m_Model.Current == node.Path;
			if (isCurrent)
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, theme.ButtonHover, 2.0f });
			const bool hovered = ctx.IsHovered(row);
			if (hovered && !isCurrent)
				ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, { 1, 1, 1, 0.06f }, 2.0f });
			if (node.HasChildren)
			{
				const Wui::WuiRect arrow { row.X, ty, 16, 20 };
				const bool open = m_Model.TreeOpen.find(node.Path) != m_Model.TreeOpen.end();
				if (ctx.IsClicked(arrow))
				{
					if (open) m_Model.TreeOpen.erase(node.Path);
					else m_Model.TreeOpen.insert(node.Path);
					SaveState();
				}
				Label(ctx, { row.X, ty + 2 }, open ? "[-]" : "[+]", theme.TextMuted, 12.0f);
			}
			const float labelX = node.HasChildren ? row.X + 18 : row.X + 2;
			if (ctx.IsClicked({ labelX, ty, row.W - (labelX - row.X), 20 }))
				Navigate(node.Path);
			if (ctx.Input().MouseDown[0] && hovered && node.Path != m_Model.Root)
			{
				const std::filesystem::path rel = node.Path.lexically_relative(m_Model.Root);
				ctx.BeginDrag(Wui::HashId(("browser.drag." + rel.string()).c_str()), "file:" + rel.string());
				ctx.SetCursor(Wui::WuiCursor::Hand);
			}
			if (fileDrag && hovered)
			{
				ctx.DropTarget(row, "file:");
				m_Model.PendingDropDest = node.Path;
				ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, row, theme.Accent, 2.0f, 2.0f });
			}
			Label(ctx, { labelX, ty + 2 }, node.Path.filename().string(), theme.Text, 13.0f);
			ty += 20;
		}
		EndScrollArea(ctx);

		// ---- 右侧内容区 ----
		const Wui::WuiRect content { rect.X + treeW + 6, y, rect.W - treeW - 6, rect.H - (y - rect.Y) };
		const bool searching = m_Model.Search[0] != 0;
		if (!searching)
			RefreshListing();
		const std::vector<std::filesystem::path>& paths = searching ? m_Model.SearchResults : m_Model.Listing;

		if (ctx.Input().Ctrl && ctx.IsKeyPressed(KeyCodes::A) && ctx.IsHovered(content))
			SelectAll(paths);
		if (ctx.IsKeyPressed(KeyCodes::Delete) && !m_Model.Selected.empty() && ctx.IsHovered(content))
			m_Model.ShowDeleteModal = true;
		if (ctx.IsKeyPressed(KeyCodes::F2) && m_Model.Selected.size() == 1 && ctx.IsHovered(content))
		{
			StartRename(ctx, *m_Model.Selected.begin());
		}

		if (fileDrag && ctx.IsHovered(content))
		{
			ctx.DropTarget(content, "file:");
			m_Model.PendingDropDest = m_Model.Current;
			ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, content, theme.Accent, 0.0f, 2.0f });
		}

		bool itemRightClicked = false;
		auto interact = [&](const std::filesystem::path& path, const Wui::WuiRect& itemRect, bool isDir)
		{
			const bool selected = m_Model.Selected.find(path) != m_Model.Selected.end();
			const bool hovered = ctx.IsHovered(itemRect);
			if (isDir && fileDrag && hovered)
			{
				ctx.DropTarget(itemRect, "file:");
				m_Model.PendingDropDest = path;
				ctx.Commands().push_back({ Wui::WuiDrawKind::RectOutline, itemRect, theme.Accent, 2.0f, 2.0f });
			}
			if (ctx.Input().MouseDown[0] && hovered)
			{
				const std::filesystem::path rel = path.lexically_relative(m_Model.Root);
				ctx.BeginDrag(Wui::HashId(("browser.drag." + rel.string()).c_str()), "file:" + rel.string());
				ctx.SetCursor(Wui::WuiCursor::Hand);
			}
			if (ctx.IsDoubleClicked(itemRect))
				OpenItem(path);
			else if (ctx.IsClicked(itemRect))
			{
				if (ctx.Input().Ctrl)
				{
					if (selected) m_Model.Selected.erase(path);
					else m_Model.Selected.insert(path);
				}
				else if (ctx.Input().Shift && !m_Model.LastSelected.empty())
				{
					auto start = std::find(paths.begin(), paths.end(), m_Model.LastSelected);
					auto end = std::find(paths.begin(), paths.end(), path);
					if (start != paths.end() && end != paths.end())
					{
						if (std::distance(start, end) < 0) std::swap(start, end);
						for (auto it = start; it <= end; ++it)
							m_Model.Selected.insert(*it);
					}
				}
				else
				{
					m_Model.Selected.clear();
					m_Model.Selected.insert(path);
				}
				m_Model.LastSelected = path;
			}
			else if (ctx.Input().MouseClicked[1] && hovered)
			{
				itemRightClicked = true;
				if (!selected)
				{
					m_Model.Selected.clear();
					m_Model.Selected.insert(path);
					m_Model.LastSelected = path;
				}
				m_Model.ContextMenuPath = path;
				m_Model.ContextMenuPos = ctx.Input().MousePos;
				ctx.OpenPopup(Wui::HashId("browser.context"));
			}
			return selected || hovered;
		};

		if (m_Model.ListMode)
		{
			const float rowH = 24;
			Label(ctx, { content.X + 8, content.Y + 4 }, "Name", theme.TextMuted, 13.0f);
			Label(ctx, { content.X + content.W * 0.52f, content.Y + 4 }, "Type", theme.TextMuted, 13.0f);
			Label(ctx, { content.X + content.W * 0.72f, content.Y + 4 }, "Size", theme.TextMuted, 13.0f);
			BeginScrollArea(ctx, { content.X, content.Y + 22, content.W, content.H - 22 }, paths.size() * rowH, m_Model.ContentScroll, theme);
			for (size_t i = 0; i < paths.size(); ++i)
			{
				const std::filesystem::path& path = paths[i];
				std::error_code dirError;
				const bool isDir = std::filesystem::is_directory(path, dirError);
				const Wui::WuiRect row { content.X + 4, content.Y + 24 + i * rowH - m_Model.ContentScroll, content.W - 8, rowH };
				interact(path, row, isDir);
				if (m_Model.Selected.find(path) != m_Model.Selected.end())
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, row, { 0.28f, 0.45f, 0.85f, 0.35f }, 2.0f });
				Ref<Texture2D> icon = isDir ? m_Model.DirIcon : m_Model.FileIcon;
				if (icon)
					Image(ctx, { row.X + 2, row.Y + 3, 18, 18 }, icon->GetRendererID(), { 0, 1, 1, -1 }, theme);
				Label(ctx, { row.X + 26, row.Y + 4 }, path.filename().string(), theme.Text, 13.0f);
				Label(ctx, { row.X + content.W * 0.52f, row.Y + 4 }, isDir ? "Folder" : "File", theme.TextMuted, 13.0f);
				std::string size = "-";
				if (!isDir)
					size = FormatBytes(static_cast<size_t>(FileSize(path)));
				Label(ctx, { row.X + content.W * 0.72f, row.Y + 4 }, size, theme.TextMuted, 13.0f);
				if (m_Model.RenameTarget == path)
					RenderRenameField(ctx, path, { row.X + 26, row.Y + 2, 160, 20 }, theme);
			}
			EndScrollArea(ctx);
		}
		else
		{
			const float cell = 142;
			const int columns = std::max(1, static_cast<int>(content.W / cell));
			const float rows = std::ceil(static_cast<float>(paths.size()) / columns);
			BeginScrollArea(ctx, content, rows * cell + 16, m_Model.ContentScroll, theme);
			for (size_t i = 0; i < paths.size(); ++i)
			{
				const int column = static_cast<int>(i % columns);
				const int row = static_cast<int>(i / columns);
				const Wui::WuiRect cellRect { content.X + 8 + column * cell, content.Y + 8 + row * cell - m_Model.ContentScroll, 128, 128 };
				const std::filesystem::path& path = paths[i];
				std::error_code dirError;
				const bool isDir = std::filesystem::is_directory(path, dirError);
				const bool selected = m_Model.Selected.find(path) != m_Model.Selected.end();
				const bool hovered = ctx.IsHovered(cellRect);
				if (selected)
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { cellRect.X - 3, cellRect.Y - 3, cellRect.W + 6, cellRect.H + 28 }, { 0.28f, 0.45f, 0.85f, 0.35f }, 4.0f });
				else if (hovered)
					ctx.Commands().push_back({ Wui::WuiDrawKind::Rect, { cellRect.X - 3, cellRect.Y - 3, cellRect.W + 6, cellRect.H + 28 }, theme.ButtonHover, 4.0f });
				interact(path, cellRect, isDir);
				Ref<Texture2D> icon = isDir ? m_Model.DirIcon : m_Model.FileIcon;
				if (icon)
					Image(ctx, cellRect, icon->GetRendererID(), { 0, 1, 1, -1 }, theme);
				Label(ctx, { cellRect.X, cellRect.Y + 130 }, path.filename().string(), theme.Text, 13.0f);
				if (m_Model.RenameTarget == path)
					RenderRenameField(ctx, path, { cellRect.X, cellRect.Y + 150, 128, 22 }, theme);
			}
			EndScrollArea(ctx);
		}

		if (ctx.AcceptDrop(&filePayload, "file:"))
		{
			if (!m_Model.PendingDropDest.empty())
			{
				const std::filesystem::path dragged = m_Model.Root / filePayload.substr(5);
				const std::filesystem::path dest = m_Model.PendingDropDest;
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
						if (m_Ctx) m_Ctx->RecordOp("browser", "move", dragged.filename().string(), "-> " + dest.string());
						if (m_Model.Selected.erase(dragged) > 0)
							m_Model.Selected.insert(movedTo);
						if (m_Model.LastSelected == dragged)
							m_Model.LastSelected = movedTo;
						if (m_Model.Current == dragged)
						{
							m_Model.Current = movedTo;
							if (m_Model.Search[0])
								UpdateSearch();
						}
						Reveal(dest);
						InvalidateContents();
						SaveState();
					}
					catch (const std::exception& error)
					{
						WLD_CORE_ERROR("Content browser move failed: {0}", error.what());
					}
				}
			}
			m_Model.PendingDropDest.clear();
		}
		else if (!fileDrag)
		{
			m_Model.PendingDropDest.clear();
		}

		// ---- 右键菜单 ----
		const Wui::WuiId popup = Wui::HashId("browser.context");
		if (ctx.IsPopupOpen(popup) && !m_Model.ContextMenuPath.empty())
		{
			ctx.PushOverlay();
			const Wui::WuiRect menuPanel { m_Model.ContextMenuPos.x, m_Model.ContextMenuPos.y, 180, 8 * 24 + 8 };
			DrawPanelSurface(ctx, menuPanel, theme);
			struct BrowserItem { const char* Label; std::function<void()> Action; };
			const bool single = m_Model.Selected.size() == 1;
			const std::vector<BrowserItem> items = {
				{ "Open", [this] { OpenItem(m_Model.ContextMenuPath); } },
				{ "Cut", [this] { Cut(); } },
				{ "Copy", [this] { Copy(); } },
				{ "Paste", [this] { PasteInto(std::filesystem::is_directory(m_Model.ContextMenuPath) ? m_Model.ContextMenuPath : m_Model.Current); } },
				{ "Rename", [this, single] { if (single && m_Ctx) StartRename(*m_Ctx, m_Model.ContextMenuPath); } },
				{ "New Folder", [this, &ctx] { CreateFolder(ctx); } },
				{ "Open in Explorer", [this] { OpenInExplorer(m_Model.ContextMenuPath); } },
				{ "Delete", [this] { m_Model.ShowDeleteModal = true; } },
			};
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Wui::WuiRect item { menuPanel.X + 4, menuPanel.Y + 4 + i * 24, menuPanel.W - 8, 22 };
				if (MenuItem(ctx, Wui::HashId(("browser.item." + std::string(items[i].Label)).c_str()), item, items[i].Label, true, theme))
				{
					items[i].Action();
					if (m_Ctx) m_Ctx->RecordOp("menu", "item", items[i].Label, "browser");
					ctx.CloseAllPopups();
				}
			}
			ctx.ClosePopupsOnOutsideClick({ popup }, menuPanel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(popup);
			ctx.PopOverlay();
		}
		else
		{
			m_Model.ContextMenuPath.clear();
			if (ctx.IsPopupOpen(popup))
				ctx.ClosePopup(popup);
		}

		// ---- 内容区空白处右键 ----
		if (ctx.Input().MouseClicked[1] && ctx.IsHovered(content) && !itemRightClicked)
		{
			m_Model.BlankMenuPos = ctx.Input().MousePos;
			ctx.OpenPopup(Wui::HashId("browser.blankcontext"));
		}
		const Wui::WuiId blankPopup = Wui::HashId("browser.blankcontext");
		if (ctx.IsPopupOpen(blankPopup))
		{
			ctx.PushOverlay();
			const Wui::WuiRect menuPanel { m_Model.BlankMenuPos.x, m_Model.BlankMenuPos.y, 180, 3 * 24 + 8 };
			DrawPanelSurface(ctx, menuPanel, theme);
			struct BlankItem { const char* Label; std::function<void()> Action; };
			const std::vector<BlankItem> items = {
				{ "New Folder", [this, &ctx] { CreateFolder(ctx); } },
				{ "Paste", [this] { PasteInto(m_Model.Current); } },
				{ "Refresh", [this] { InvalidateContents(); if (m_Model.Search[0]) UpdateSearch(); } },
			};
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Wui::WuiRect item { menuPanel.X + 4, menuPanel.Y + 4 + i * 24, menuPanel.W - 8, 22 };
				if (MenuItem(ctx, Wui::HashId(("browser.blank." + std::string(items[i].Label)).c_str()), item, items[i].Label, true, theme))
				{
					items[i].Action();
					if (m_Ctx) m_Ctx->RecordOp("menu", "item", items[i].Label, "browser-blank");
					ctx.CloseAllPopups();
				}
			}
			ctx.ClosePopupsOnOutsideClick({ blankPopup }, menuPanel);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
				ctx.ClosePopup(blankPopup);
			ctx.PopOverlay();
		}

		// ---- 删除确认 ----
		const Wui::WuiId deleteModal = Wui::HashId("browser.delete");
		if (m_Model.ShowDeleteModal && (ctx.Modal() == 0 || ctx.Modal() == deleteModal))
			ctx.SetModal(deleteModal);
		else if (!m_Model.ShowDeleteModal && ctx.Modal() == deleteModal)
			ctx.ClearModal();
		Wui::WuiRect panel;
		if (BeginModal(ctx, deleteModal, "Delete Confirmation", { 380, 160 }, &panel, theme))
		{
			Label(ctx, { panel.X + 16, panel.Y + 48 }, "Delete " + std::to_string(m_Model.Selected.size()) + " item(s)? This cannot be undone.", theme.Text, 14.0f);
			if (Button(ctx, Wui::HashId("browser.delete.yes"), { panel.X + 20, panel.Y + 110, 110, 28 }, "Yes", theme))
			{
				DeleteSelection();
				m_Model.ShowDeleteModal = false;
				ctx.ClearModal();
			}
			if (Button(ctx, Wui::HashId("browser.delete.no"), { panel.X + 150, panel.Y + 110, 110, 28 }, "No", theme))
			{
				m_Model.ShowDeleteModal = false;
				ctx.ClearModal();
			}
			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				m_Model.ShowDeleteModal = false;
				ctx.ClearModal();
			}
			EndModal(ctx, deleteModal);
		}
	}
}
