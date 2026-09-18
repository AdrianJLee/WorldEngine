#include "wldpch.h"
#include "ContentBrowserPanel.h"

#include "World/Core/KeyCodes.h"
#include "World/WUI/WuiJson.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiTextureRegistry.h"
#include "World/WUI/Widgets/WuiChrome.h"
#include "World/Renderer/Texture.h"
#include "World/Renderer/Material.h"

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
		else if (path.extension() == ".wmat")
		{
			// D3:材质资产双击 → 材质编辑器(独立窗口)载入。
			// 面板/渲染侧都按"相对 Game/assets"的路径引用,这里转成同一约定。
			const std::filesystem::path contentRoot = m_Model.Root;
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, contentRoot, ec);
			m_Host.OpenMaterialEditor(ec ? path.generic_string() : relative.generic_string());
		}
		else if (path.extension() == ".lua" || path.extension() == ".luau")
		{
			// W9-2:脚本双击 → 内置脚本编辑器(与双击材质同一条路:逻辑路径相对 Game/assets,
			// 默认附加到主窗口;解析失败时面板自身显示只读 + 错误文本)。
			const std::filesystem::path contentRoot = m_Model.Root;
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, contentRoot, ec);
			m_Host.OpenScriptEditor(ec ? path.generic_string() : relative.generic_string());
		}
		else if (path.extension() == ".wmodel")
		{
			// P1b D5:模型双击 → **打开只读预览**(不改场景);要放进场景在预览里点按钮。
			// 用户反馈"多次打开会多次叠加":打开动作不应有场景副作用。
			const std::filesystem::path contentRoot = m_Model.Root;
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, contentRoot, ec);
			const std::string logical = ec ? path.generic_string() : relative.generic_string();
			m_Host.OpenModelPreview(logical);
			WLD_CORE_INFO("[model] preview opened: {0}", logical);
		}
		else if (path.extension() == ".gltf" || path.extension() == ".glb")
		{
			// P1b D5:glTF 双击 → 导入(.wmodel/.wmat/贴图)并**打开模型预览**(不实例化)。
			// 导入后立刻刷新列表,新产出的 .wmodel/.wmat 马上可见。
			std::string message;
			std::string logicalModel;
			if (!m_Host.ImportModelFile(path.string(), &message, &logicalModel))
				WLD_CORE_WARN("[model] import '{0}' failed: {1}", path.string(), message);
			else
			{
				WLD_CORE_INFO("[model] {0}", message);
				InvalidateContents();
				if (m_Model.Search[0])
					UpdateSearch();
				if (!logicalModel.empty())
					m_Host.OpenModelPreview(logicalModel);
			}
		}
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

	void ContentBrowserPanel::CreateMaterial(Wui::WuiContext& ctx)
	{
		// 新建材质资产:写一份默认 .wmat 模板到当前目录(重名自动编号),随后直接在材质编辑器里打开。
		// 内容根 = 内容浏览器 Root;材质路径按"相对内容根"书写,与渲染侧引用约定一致。
		std::filesystem::path target = m_Model.Current / "material.wmat";
		int counter = 1;
		while (std::filesystem::exists(target))
			target = m_Model.Current / ("material (" + std::to_string(counter++) + ").wmat");

		MaterialDesc desc;
		desc.Name = target.stem().string();
		std::string error;
		if (!MaterialIO::WriteFileText(
			std::filesystem::relative(target, m_Model.Root).generic_string(),
			MaterialIO::Serialize(desc), &error))
		{
			WLD_CORE_ERROR("Could not create material: {0}", error);
			return;
		}
		m_Model.Selected.clear();
		m_Model.Selected.insert(target);
		m_Model.LastSelected = target;
		InvalidateContents();
		SaveState();
		if (m_Ctx) m_Ctx->RecordOp("browser", "new-material", target.filename().string(), "");
		OpenItem(target);
		(void)ctx;
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
		// 窗口/GL 上下文重建后旧图标纹理失效:丢弃缓存,重新加载并注册。
		if (m_TextureEpoch != host.TextureEpoch())
		{
			m_TextureEpoch = host.TextureEpoch();
			m_DirIcon = nullptr;
			m_FileIcon = nullptr;
			m_DirIconId = 0;
			m_FileIconId = 0;
		}
		if (!m_DirIcon)
			m_DirIcon = Texture2D::Create("Resource/Icons/ContentBrowser/DirectoryIcon.png");
		if (!m_FileIcon)
			m_FileIcon = Texture2D::Create("Resource/Icons/ContentBrowser/FileIcon.png");
		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		if (registry.Generation() != m_IconGeneration)
		{
			m_DirIconId = registry.RegisterTexture2D(m_DirIcon);
			m_FileIconId = registry.RegisterTexture2D(m_FileIcon);
			m_IconGeneration = registry.Generation();
		}

		std::string filePayload;
		const bool fileDrag = ctx.IsDragActive(&filePayload) && filePayload.rfind("file:", 0) == 0;

		// ---- 工具栏(布局树):后退/前进/上级 + 面包屑 + 视图/新建/刷新/搜索 ----
		if (!m_Toolbar)
		{
			m_Toolbar = std::make_shared<Wui::WuiBox>();
			m_Toolbar->Direction = Wui::WuiDirection::Row;
			m_Toolbar->Gap = 6;
			m_Toolbar->AlignCross = Wui::WuiAlign::Center;

			auto addButton = [&](const std::string& label, std::function<void()> action, float width)
			{
				auto button = std::make_shared<Wui::WuiButton>();
				button->Label = label;
				button->OnClick = std::move(action);
				m_Toolbar->Add(button, { width, width, 0, 24, 0 });
				return button;
			};
			m_BackButton = addButton("<", [this] { if (m_Model.HistoryIndex > 0) GoBack(); }, 24);
			m_ForwardButton = addButton(">", [this]
				{
					if (m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) - 1)
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
				}, 24);
			m_UpButton = addButton("Up", [this] { if (m_Model.Current != m_Model.Root) GoUp(); }, 32);

			m_Breadcrumbs = std::make_shared<Wui::WuiBox>();
			m_Breadcrumbs->Direction = Wui::WuiDirection::Row;
			m_Breadcrumbs->Gap = 4;
			m_Breadcrumbs->AlignCross = Wui::WuiAlign::Center;
			m_Toolbar->Add(m_Breadcrumbs);

			auto spacer = std::make_shared<Wui::WuiSpacer>();
			m_Toolbar->Add(spacer, { 0, 1e30f, 0, 1e30f, 1 });

			m_ViewModeButton = addButton(m_Model.ListMode ? "Grid" : "List", [this]
				{
					m_Model.ListMode = !m_Model.ListMode;
					SaveState();
				}, 58);
			addButton("+ Folder", [this, &ctx] { CreateFolder(ctx); }, 70);
			addButton("Refresh", [this]
				{
					InvalidateContents();
					if (m_Model.Search[0])
						UpdateSearch();
				}, 58);

			m_SearchField = std::make_shared<Wui::WuiTextField>();
			m_SearchField->Buffer = &m_Model.SearchEdit;
			m_SearchField->OnCommit = [this]
				{
					std::strncpy(m_Model.Search, m_Model.SearchEdit.c_str(), sizeof(m_Model.Search) - 1);
					m_Model.Search[sizeof(m_Model.Search) - 1] = 0;
					UpdateSearch();
				};
			m_Toolbar->Add(m_SearchField, { 162, 162, 0, 24, 0 });
		}

		m_BackButton->Enabled = m_Model.HistoryIndex > 0;
		m_ForwardButton->Enabled = m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) - 1;
		m_UpButton->Enabled = m_Model.Current != m_Model.Root;
		m_ViewModeButton->Label = m_Model.ListMode ? "Grid" : "List";

		// 面包屑随路径变化重建。
		if (m_LastCrumbPath != m_Model.Current)
		{
			m_LastCrumbPath = m_Model.Current;
			m_CrumbButtons.clear();
			m_CrumbDests.clear();
			m_Breadcrumbs->Clear();
			const auto addCrumb = [&](const std::string& label, const std::filesystem::path& destination)
			{
				auto button = std::make_shared<Wui::WuiButton>();
				button->Label = label;
				button->OnClick = [this, destination] { Navigate(destination); };
				m_Breadcrumbs->Add(button, { static_cast<float>(label.size() * 8 + 20), static_cast<float>(label.size() * 8 + 20), 0, 24, 0 });
				m_CrumbButtons.push_back(button);
				m_CrumbDests.push_back(destination);
			};
			addCrumb("Root", m_Model.Root);
			std::filesystem::path accumulated = m_Model.Root;
			for (const auto& part : m_Model.Current.lexically_relative(m_Model.Root))
			{
				accumulated /= part;
				addCrumb(part.string(), accumulated);
			}
		}

		Wui::LayoutWidgetTree(m_Toolbar, { rect.X + 6, rect.Y + 6, rect.W - 12, 24 });
		Wui::WuiPaintContext toolbarPaint(ctx);
		m_Toolbar->Paint(toolbarPaint);

		if (fileDrag)
			for (size_t i = 0; i < m_CrumbButtons.size(); ++i)
				if (ctx.IsHovered(m_CrumbButtons[i]->Rect()))
				{
					ctx.DropTarget(m_CrumbButtons[i]->Rect(), "file:");
					m_Model.PendingDropDest = m_CrumbDests[i];
					Wui::HighlightOutline(ctx, m_CrumbButtons[i]->Rect(), theme.Accent, 2.0f, 2.0f);
				}

		float y = rect.Y + 38;

		// ---- 左侧目录树 ----
		const float treeW = 190;
		const Wui::WuiRect treeRect { rect.X, y, treeW, rect.H - (y - rect.Y) };
		Wui::PanelBackground(ctx, treeRect, { 0.09f, 0.095f, 0.10f, 1 });
		RefreshTree(false);
		// 目录树走 TreeView 组件:展开箭头/悬停/选中由组件绘制,
		// 导航、拖拽起手、拖入目标仍由面板处理(用组件返回的 ItemRects)。
		std::vector<const BrowserDirNode*> visibleNodes;
		std::vector<Wui::TreeViewItem> treeItems;
		for (const BrowserDirNode& node : m_Model.DirTree)
		{
			if (node.Depth > 1 && m_Model.TreeOpen.find(node.Path.parent_path()) == m_Model.TreeOpen.end())
				continue;
			const std::filesystem::path rel = node.Path.lexically_relative(m_Model.Root);
			Wui::TreeViewItem item;
			item.Id = Wui::HashId(("browser.tree." + rel.generic_string()).c_str());
			item.Label = node.Path.filename().string();
			item.Depth = node.Depth;
			item.HasChildren = node.HasChildren;
			item.Expanded = m_Model.TreeOpen.find(node.Path) != m_Model.TreeOpen.end();
			item.Selected = m_Model.Current == node.Path;
			visibleNodes.push_back(&node);
			treeItems.push_back(std::move(item));
		}
		{
			Wui::TreeViewResult tree = Wui::TreeView(ctx, treeRect, treeItems, 20.0f, m_Model.TreeScroll, theme);
			for (size_t i = 0; i < visibleNodes.size(); ++i)
			{
				const BrowserDirNode& node = *visibleNodes[i];
				if (i >= tree.ItemRects.size())
					break;
				const Wui::WuiRect& row = tree.ItemRects[i];
				const bool hovered = ctx.IsHovered(row);
				if (tree.ClickedArrow == static_cast<int>(i))
				{
					if (treeItems[i].Expanded) m_Model.TreeOpen.erase(node.Path);
					else m_Model.TreeOpen.insert(node.Path);
					SaveState();
				}
				else if (tree.Clicked == static_cast<int>(i))
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
					Wui::HighlightOutline(ctx, row, theme.Accent, 2.0f, 2.0f);
				}
			}
		}

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
			Wui::HighlightOutline(ctx, content, theme.Accent, 0.0f, 2.0f);
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
				Wui::HighlightOutline(ctx, itemRect, theme.Accent, 2.0f, 2.0f);
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
			Label(ctx, { content.X + 8, content.Y + 4 }, "Name / Type / Size", theme.TextMuted, 13.0f);
			// 列表模式走 ListView 组件;拖拽/选中/重命名仍由面板处理——
			// 组件返回每行矩形(ItemRects),面板据此调用既有 interact/RenderRenameField。
			std::vector<Wui::ListViewItem> items;
			items.reserve(paths.size());
			for (const std::filesystem::path& path : paths)
			{
				std::error_code dirError;
				const bool isDir = std::filesystem::is_directory(path, dirError);
				std::string size = "-";
				if (!isDir)
					size = FormatBytes(static_cast<size_t>(FileSize(path)));
				const std::filesystem::path rel = path.lexically_relative(m_Model.Root);
				Wui::ListViewItem item;
				item.Id = Wui::HashId(("browser.item." + rel.generic_string()).c_str());
				item.Label = path.filename().string();
				item.SubLabel = isDir ? "Folder" : size;
				item.Icon = isDir ? m_DirIconId : m_FileIconId;
				item.Uv = { 0, 1, 1, -1 };
				item.Selected = m_Model.Selected.find(path) != m_Model.Selected.end();
				items.push_back(std::move(item));
			}
			const Wui::ListViewResult lv = Wui::ListView(ctx,
				{ content.X, content.Y + 22, content.W, content.H - 22 }, items, rowH, m_Model.ContentScroll, theme);
			for (size_t i = 0; i < paths.size() && i < lv.ItemRects.size(); ++i)
			{
				std::error_code dirError;
				const bool isDir = std::filesystem::is_directory(paths[i], dirError);
				interact(paths[i], lv.ItemRects[i], isDir);
				if (m_Model.RenameTarget == paths[i])
					RenderRenameField(ctx, paths[i], { lv.ItemRects[i].X + 26, lv.ItemRects[i].Y + 2, 160, 20 }, theme);
			}
		}
		else
		{
			const float cell = 142;
			// 网格模式走 GridView 组件(缩略图/选中/悬停由组件绘制);
			// 拖拽/选中/重命名仍由面板处理(使用组件返回的 ItemRects)。
			std::vector<Wui::GridViewItem> items;
			items.reserve(paths.size());
			for (const std::filesystem::path& path : paths)
			{
				std::error_code dirError;
				const bool isDir = std::filesystem::is_directory(path, dirError);
				const std::filesystem::path rel = path.lexically_relative(m_Model.Root);
				Wui::GridViewItem item;
				item.Id = Wui::HashId(("browser.cell." + rel.generic_string()).c_str());
				item.Label = path.filename().string();
				item.Icon = isDir ? m_DirIconId : m_FileIconId;
				item.Uv = { 0, 1, 1, -1 };
				item.Selected = m_Model.Selected.find(path) != m_Model.Selected.end();
				items.push_back(std::move(item));
			}
			const Wui::GridViewResult gv = Wui::GridView(ctx, content, items, cell, cell + 30.0f,
				m_Model.ContentScroll, theme);
			for (size_t i = 0; i < paths.size() && i < gv.ItemRects.size(); ++i)
			{
				std::error_code dirError;
				const bool isDir = std::filesystem::is_directory(paths[i], dirError);
				interact(paths[i], gv.ItemRects[i], isDir);
				if (m_Model.RenameTarget == paths[i])
					RenderRenameField(ctx, paths[i],
						{ gv.ItemRects[i].X, gv.ItemRects[i].Y + gv.ItemRects[i].H + 2.0f, 128, 22 }, theme);
			}
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
			struct BrowserItem { const char* Label; std::function<void()> Action; };
			const bool single = m_Model.Selected.size() == 1;
			const std::vector<BrowserItem> items = {
				{ "Open", [this] { OpenItem(m_Model.ContextMenuPath); } },
				{ "Cut", [this] { Cut(); } },
				{ "Copy", [this] { Copy(); } },
				{ "Paste", [this] { PasteInto(std::filesystem::is_directory(m_Model.ContextMenuPath) ? m_Model.ContextMenuPath : m_Model.Current); } },
				{ "Rename", [this, single] { if (single && m_Ctx) StartRename(*m_Ctx, m_Model.ContextMenuPath); } },
				{ "New Folder", [this, &ctx] { CreateFolder(ctx); } },
				{ "New Material", [this, &ctx] { CreateMaterial(ctx); } },
				{ "Open in Explorer", [this] { OpenInExplorer(m_Model.ContextMenuPath); } },
				{ "Delete", [this] { m_Model.ShowDeleteModal = true; } },
			};
			// 右键菜单走组件(ContextMenu):位置钉住 + 外部点击/Esc 关闭统一处理。
			Wui::WuiRect menuPanel;
			if (Wui::BeginContextMenu(ctx, popup, m_Model.ContextMenuPos, 180.0f, items.size(), &menuPanel, theme))
			{
				for (size_t i = 0; i < items.size(); ++i)
				{
					const Wui::WuiRect item { menuPanel.X + 4, menuPanel.Y + 4 + i * 22, menuPanel.W - 8, 22 };
					if (Wui::ContextMenuItem(ctx, Wui::HashId(("browser.item." + std::string(items[i].Label)).c_str()),
						item, items[i].Label, theme))
					{
						items[i].Action();
						if (m_Ctx) m_Ctx->RecordOp("menu", "item", items[i].Label, "browser");
						ctx.CloseAllPopups();
					}
				}
				if (ctx.IsKeyPressed(KeyCodes::Escape))
					ctx.ClosePopup(popup);
				Wui::EndContextMenu(ctx, popup, menuPanel, theme);
			}
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
			const Wui::WuiRect menuPanel { m_Model.BlankMenuPos.x, m_Model.BlankMenuPos.y, 180, 4 * 24 + 8 };
			DrawPanelSurface(ctx, menuPanel, theme);
			struct BlankItem { const char* Label; std::function<void()> Action; };
			const std::vector<BlankItem> items = {
				{ "New Folder", [this, &ctx] { CreateFolder(ctx); } },
				{ "New Material", [this, &ctx] { CreateMaterial(ctx); } },
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

