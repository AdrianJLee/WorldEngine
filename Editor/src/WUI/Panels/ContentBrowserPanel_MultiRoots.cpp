#include "ContentBrowserPanel_Internal.h"

namespace World
{

using namespace ContentBrowserPanelDetail;


	// ---- CPPSRC-1/PLUG-T3:多根(内容根 ⇄ 项目 C++ 源码根 ⇄ 项目插件根)----

std::string ContentBrowserPanel::ScopeKey(BrowserRootScope scope){
		switch (scope)
		{
			case BrowserRootScope::ProjectSources: return "projectSources";
			case BrowserRootScope::ProjectPlugins: return "projectPlugins";
			default: return "content";
		}
	}


std::filesystem::path ContentBrowserPanel::RootPathFor(BrowserRootScope scope) const{
		switch (scope)
		{
			case BrowserRootScope::ProjectSources: return m_Model.SourceRoot.empty()
				? ProjectSourceRoot() : m_Model.SourceRoot;
			case BrowserRootScope::ProjectPlugins: return m_Model.PluginsRoot.empty()
				? ProjectPluginsRoot() : m_Model.PluginsRoot;
			default: return m_Model.ContentRoot;
		}
	}


std::filesystem::path ContentBrowserPanel::ProjectSourceRoot() const{
		const std::filesystem::path projectRoot = World::Paths::ProjectDir();
		if (projectRoot.empty())
			return {};
		return projectRoot / "src";
	}


std::filesystem::path ContentBrowserPanel::ProjectPluginsRoot() const{
		const std::filesystem::path projectRoot = World::Paths::ProjectDir();
		if (projectRoot.empty())
			return {};
		return projectRoot / "plugins";
	}


bool ContentBrowserPanel::ProjectPluginsAvailable() const{
		const std::filesystem::path root = ProjectPluginsRoot();
		const double now = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		if (root == m_PluginsRootChecked && m_PluginsRootCheckedAt >= 0.0
			&& now - m_PluginsRootCheckedAt < 0.5)
			return m_PluginsRootAvailable;
		m_PluginsRootChecked = root;
		m_PluginsRootCheckedAt = now;
		std::error_code dirError;
		m_PluginsRootAvailable = !root.empty() && std::filesystem::is_directory(root, dirError);
		return m_PluginsRootAvailable;
	}


bool ContentBrowserPanel::ProjectSourcesAvailable() const{
		const std::filesystem::path root = ProjectSourceRoot();
		const double now = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		// 项目根变了(重新打开项目)立刻重算,不吃 0.5s 缓存的亏;否则 0.5s 节流。
		if (root == m_SourceRootChecked && m_SourceRootCheckedAt >= 0.0
			&& now - m_SourceRootCheckedAt < 0.5)
			return m_SourceRootAvailable;
		m_SourceRootChecked = root;
		m_SourceRootCheckedAt = now;
		std::error_code dirError;
		m_SourceRootAvailable = !root.empty() && std::filesystem::is_directory(root, dirError);
		return m_SourceRootAvailable;
	}


void ContentBrowserPanel::CaptureRootState(BrowserRootScope scope){
		const int index = static_cast<int>(scope);
		RootState& state = m_RootStates[index];
		state.Current = m_Model.Current;
		state.TreeOpen = m_Model.TreeOpen;
		state.Selected = m_Model.Selected;
		state.LastSelected = m_Model.LastSelected;
		state.History = m_Model.History;
		state.HistoryIndex = m_Model.HistoryIndex;
		state.TreeScroll = m_Model.TreeScroll;
		state.ContentScroll = m_Model.ContentScroll;
		state.DirTree = m_Model.DirTree;
		state.DirTreeDirty = m_Model.DirTreeDirty;
		state.TreeStamp = m_Model.TreeStamp;
		state.LastTreeCheck = m_Model.LastTreeCheck;
		state.ListingPath = m_Model.ListingPath;
		state.Listing = m_Model.Listing;
		state.ListingDirty = m_Model.ListingDirty;
		state.ListingStamp = m_Model.ListingStamp;
		state.LastListingCheck = m_Model.LastListingCheck;
		state.SizeCache = m_Model.SizeCache;
		state.SearchEdit = m_Model.SearchEdit;
		state.SearchResults = m_Model.SearchResults;
		state.Search = std::string(m_Model.Search);
		m_RootStatesReady[index] = true;
	}


void ContentBrowserPanel::ApplyRootState(BrowserRootScope scope){
		const int index = static_cast<int>(scope);
		RootState& state = m_RootStates[index];
		if (!m_RootStatesReady[index])
		{
			// 第一次进这个根:从干净状态起步(位置 = 根本身),不继承另一个根的浏览位置。
			m_Model.Current = m_Model.Root;
			m_Model.TreeOpen.clear();
			m_Model.TreeOpen.insert(m_Model.Root);
			m_Model.Selected.clear();
			m_Model.LastSelected.clear();
			m_Model.History.clear();
			m_Model.HistoryIndex = -1;
			m_Model.TreeScroll = 0.0f;
			m_Model.ContentScroll = 0.0f;
			m_Model.SizeCache.clear();
			m_Model.SearchEdit.clear();
			m_Model.Search[0] = '\0';
			m_Model.SearchResults.clear();
			return;
		}
		std::error_code dirError;
		m_Model.Current = (!state.Current.empty() && std::filesystem::is_directory(state.Current, dirError))
			? state.Current : m_Model.Root;
		m_Model.TreeOpen = state.TreeOpen;
		m_Model.Selected = state.Selected;
		m_Model.LastSelected = state.LastSelected;
		m_Model.History = state.History;
		m_Model.HistoryIndex = state.HistoryIndex;
		m_Model.TreeScroll = state.TreeScroll;
		m_Model.ContentScroll = state.ContentScroll;
		m_Model.DirTree = state.DirTree;
		m_Model.DirTreeDirty = true;   // 树按新根重建(路径是绝对的,但深度/子项缓存可能过期)
		m_Model.TreeStamp = {};
		m_Model.LastTreeCheck = {};
		m_Model.ListingPath = state.ListingPath;
		m_Model.Listing = state.Listing;
		m_Model.ListingDirty = true;
		m_Model.ListingStamp = {};
		m_Model.LastListingCheck = {};
		m_Model.SizeCache = state.SizeCache;
		m_Model.SearchEdit = state.SearchEdit;
		std::snprintf(m_Model.Search, sizeof(m_Model.Search), "%s", state.Search.c_str());
		m_Model.SearchResults = state.SearchResults;
	}


bool ContentBrowserPanel::SwitchRoot(RootScope scope){
		// 各根的路径每次切换都按运行期重新解析(用户可能刚换了项目)。
		m_Model.ContentRoot = World::Paths::AssetRoot();
		m_Model.SourceRoot = ProjectSourceRoot();
		m_Model.PluginsRoot = ProjectPluginsRoot();
		if (scope == BrowserRootScope::ProjectSources && !ProjectSourcesAvailable())
			return false;   // 没项目 / 没有 <项目根>/src:调用方给可读提示,状态一律不动
		if (scope == BrowserRootScope::ProjectPlugins && !ProjectPluginsAvailable())
			return false;   // 没项目 / 没有 <项目根>/plugins:同上(与项目 C++ 根同口径)
		if (scope == m_Model.Scope)
			return true;    // 幂等
		CaptureRootState(m_Model.Scope);
		m_Model.Scope = scope;
		m_Model.Root = RootPathFor(scope);
		// 瞬态:切根不带走另一个根的右键菜单/重命名/删除确认/剪贴板/新建文件夹输入。
		m_Model.ContextMenuPath.clear();
		m_Model.ContextMenuPos = {};
		m_Model.BlankMenuPos = {};
		m_Model.RenameActive = false;
		m_Model.RenameTarget.clear();
		m_Model.RenameEdit.clear();
		m_Model.RenameBuffer[0] = '\0';
		m_Model.ShowDeleteModal = false;
		m_Model.ShowNewFolderInput = false;
		m_Model.NewFolderBuffer[0] = '\0';
		m_Model.Clipboard.clear();
		m_Model.ClipboardCut = false;
		m_Model.PendingDropDest.clear();
		// PLUG-T3:切根顺手收起"新建"子菜单(它属于上一个根;切根后不应再把上一个根的清单画出来)。
		m_NewMenuOwner = 0;
		m_TreeMenuPath.clear();
		ApplyRootState(scope);
		InvalidateContents();
		RefreshTree(true);
		if (m_Model.Search[0])
			UpdateSearch();
		SaveState();
		return true;
	}


bool ContentBrowserPanel::RevealPathInRoot(RootScope scope, const std::filesystem::path& absolutePath){
		if (absolutePath.empty())
			return false;
		if (!SwitchRoot(scope))
			return false;   // 根不可用 / 没有项目:状态一律不动
		std::error_code error;
		if (!std::filesystem::exists(absolutePath, error))
			return false;
		// 目标必须真的落在当前根下(不做跨根/越界跳转;plugins 根之外的引擎插件不在本根里)。
		const std::filesystem::path relative = absolutePath.lexically_relative(m_Model.Root);
		if (relative.empty() || *relative.begin() == "..")
			return false;
		// 定位到目标的父目录并选中目标本身(目录 = 在网格里选中该目录,资源管理器同款)。
		const std::filesystem::path navigateTo = absolutePath.parent_path();
		if (!navigateTo.empty() && navigateTo != m_Model.Current
			&& navigateTo.lexically_relative(m_Model.Root) != std::filesystem::path("..")
			&& std::filesystem::is_directory(navigateTo, error))
			Navigate(navigateTo);
		m_Model.Selected.clear();
		m_Model.Selected.insert(absolutePath);
		m_Model.LastSelected = absolutePath;
		Reveal(absolutePath);
		InvalidateContents();
		SaveState();
		if (m_Model.Search[0])
			UpdateSearch();
		return true;
	}


void ContentBrowserPanel::Navigate(const std::filesystem::path& path){
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


void ContentBrowserPanel::Reveal(const std::filesystem::path& path){
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


void ContentBrowserPanel::GoBack(){
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


void ContentBrowserPanel::GoUp(){
		if (m_Model.Current != m_Model.Root)
			Navigate(m_Model.Current.parent_path());
	}


void ContentBrowserPanel::OpenItem(const std::filesystem::path& path){
		if (std::filesystem::is_directory(path))
		{
			Navigate(path);
			return;
		}
		// CPPSRC-1:项目层 C++ 源码(.h/.cpp)双击 ⇒ **外部 Visual Studio** —— 与 Scripts 面板的
		// "Open in VS" 同一条 PanelHost 路径(EditorShell::OpenScriptEditorNow 按扩展名分流;
		// 内置脚本编辑器只服务 Lua/Luau)。路径按**绝对路径**给,不参与内容根相对逻辑路径换算。
		{
			const EditorAssetKind kind = DescribeAssetType(path, false).Kind;
			if (kind == EditorAssetKind::CppHeader || kind == EditorAssetKind::CppSource)
			{
				m_Host.OpenScriptEditor(path.generic_string());
				if (m_Ctx)
					m_Ctx->RecordOp("browser", "open-cpp", path.filename().generic_string(),
						path.generic_string());
				return;
			}
			// PLUG-T3:项目插件根下的文件同样"像 C++ 代码一样"打开 —— 插件清单
			// (`plugin.we.yaml`)不进内置脚本编辑器,直接走 Visual Studio(同一实例复用路径)。
			if (m_Model.Scope == BrowserRootScope::ProjectPlugins && IsPluginManifestPath(path))
			{
				if (auto* shell = dynamic_cast<EditorShell*>(&m_Host))
					shell->OpenInVisualStudioNow(path);
				if (m_Ctx)
					m_Ctx->RecordOp("browser", "open-plugin-manifest", path.filename().generic_string(),
						path.generic_string());
				return;
			}
		}
		if (path.extension() == ".wd")
			m_Host.OpenScene(path);
		else if (LowerExtension(path) == ".wtex")
		{
			// M4-TEX P4:纹理**资产**双击 → 打开 Texture Settings(编辑设置 + source:,
			// Apply 保存 `.wtex` 并就地重烘 `<源图>.wtexc`)。源图行不承担设置入口。
			OpenTextureSettingsFor(path, false);
		}
		else if (IsTextureSourcePath(path))
		{
			// M4-TEX P5(用户 2026-09-25:「对于原始图片格式,双击自动创建」):双击源图 = **导入** ——
			// 缺同主名 `.wtex` 就先写一份最小资产(默认字段,不写 `source:`;缺省就是同目录同主名),
			// 然后打开 Texture Settings。已有资产 = 直接打开(不覆盖用户的设置)。
			std::string logical;
			if (!LogicalPathFor(path, &logical))
			{
				NotifyAssetFailure(Wui::Tr("panel.content_browser.texture.outside_root",
					"Texture settings need an asset inside the content root."));
			}
			else
			{
				std::string assetLogical;
				bool created = false;
				std::string error;
				if (!Editor::EnsureTextureAssetForSource(m_Model.Root, logical, &assetLogical, &created,
						error))
				{
					NotifyAssetFailure(error);
					WLD_CORE_WARN("[texture] double-click import failed for '{0}': {1}", logical, error);
				}
				else
				{
					InvalidateContents();
					m_TextureBadges.erase(path);   // 徽标立刻重算("默认设置" → "有资产")
					if (created)
					{
						WLD_CORE_INFO("[texture] double-click created texture asset: {0}", assetLogical);
						if (m_Ctx)
							m_Ctx->RecordOp("browser", "create-texture-asset", logical, assetLogical);
						m_Host.Notify(Wui::TrFormat(
							"panel.content_browser.notice.texture_asset_created", "Created {asset}",
							{ { "asset", assetLogical } }));
					}
					OpenTextureSettingsFor(m_Model.Root / assetLogical, false);
				}
			}
		}
		else if (path.extension() == ".wmat")
		{
			// D3:材质资产双击 → 材质编辑器(独立窗口)载入。
			// 面板/渲染侧都按"相对内容根"的路径引用,这里转成同一约定。
			const std::filesystem::path contentRoot = m_Model.Root;
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, contentRoot, ec);
			m_Host.OpenMaterialEditor(ec ? path.generic_string() : relative.generic_string());
		}
		else if (LowerExtension(path) == ".slang")
		{
			// Slang-B1:`.slang`(Material Shader)双击 → **同一个材质编辑器的代码形态**
			// (左预览 / 中代码 / 右参数);面板按扩展名决定形态,宿主入口与 .wmat 相同。
			const std::filesystem::path contentRoot = m_Model.Root;
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, contentRoot, ec);
			m_Host.OpenMaterialEditor(ec ? path.generic_string() : relative.generic_string());
		}
		else if (path.extension() == ".lua" || path.extension() == ".luau")
		{
			// W9-2:脚本双击 → 内置脚本编辑器(与双击材质同一条路:逻辑路径相对内容根,
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
		else if (path.extension() == ".wprefab")
		{
			// P4-U13c:双击 = 打开 prefab **资产窗口**(看/管理:实体树 + 组件摘要 + 引用资产 +
			// 场景实例),与 .wmodel 预览窗口同款、默认附加到主窗口。
			// 真正要改资产走窗口里的 `Edit Prefab`(文档会话,那里才有 3D 视口与 gizmo)。
			const std::filesystem::path contentRoot = m_Model.Root;
			std::error_code ec;
			const std::filesystem::path relative = std::filesystem::relative(path, contentRoot, ec);
			const std::string logical = ec ? path.generic_string() : relative.generic_string();
			m_Host.OpenPrefabWindow(logical);
			WLD_CORE_INFO("[prefab] asset window opened: {0}", logical);
		}
		else if (path.extension() == ".gltf" || path.extension() == ".glb")
		{
			// P1b D5:glTF 双击 → 导入(.wmodel/.wmat/贴图)并**打开模型预览**(不实例化)。
			// 导入后立刻刷新列表,新产出的 .wmodel/.wmat 马上可见。
			std::string message;
			std::string logicalModel;
			// D10:导入到**当前文件夹**(用户 Q1=方案 A:导入物落在当前目录里,
			// 材质/贴图在它的 materials//textures/ 子目录)。
			std::error_code destError;
			const std::filesystem::path destRelative =
				std::filesystem::relative(m_Model.Current, m_Model.Root, destError);
			const std::string destination = destError ? std::string() : destRelative.generic_string();
			if (!m_Host.ImportModelFileTo(path.string(), destination, &message, &logicalModel))
				WLD_CORE_WARN("[model] import '{0}' failed: {1}", path.string(), message);
			else
			{
				WLD_CORE_INFO("[model] {0}", message);
				InvalidateContents();
				if (m_Model.Search[0])
					UpdateSearch();
				if (!logicalModel.empty())
				{
					// P4-U10:导入完成后**选中产物**(.wmodel)—— 让用户一眼看到"真正可引用的是它",
					// 而不是继续盯着 .gltf 源文件。
					SelectCreated(m_Model.Root / logicalModel, "import-model");
					m_Host.OpenModelPreview(logicalModel);
				}
			}
		}
		else
		{
			const std::string cmd = "start \"\" \"" + std::filesystem::absolute(path).string() + "\"";
			system(cmd.c_str());
		}
	}


void ContentBrowserPanel::PasteInto(const std::filesystem::path& destination){
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


void ContentBrowserPanel::DeleteSelection(){
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
			m_TreeRenameTarget.clear();
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


void ContentBrowserPanel::CreateFolder(Wui::WuiContext& ctx){
		CreateFolderIn(ctx, m_Model.Current);
	}


std::filesystem::path ContentBrowserPanel::CreateFolderIn(Wui::WuiContext& ctx, const std::filesystem::path& parentDir){
		std::filesystem::path newPath = parentDir / "New Folder";
		int counter = 1;
		while (std::filesystem::exists(newPath))
			newPath = parentDir / ("New Folder (" + std::to_string(counter++) + ")");
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
			return newPath;
		}
		catch (const std::exception& error)
		{
			WLD_CORE_ERROR("Could not create directory: {0}", error.what());
			return {};
		}
	}

}
