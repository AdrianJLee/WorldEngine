#include "ContentBrowserPanel_Internal.h"

namespace World
{

using namespace ContentBrowserPanelDetail;


	// ---- M4-TEX P4:纹理资产(`.wtex`)与源图 ----

bool ContentBrowserPanel::LogicalPathFor(const std::filesystem::path& path, std::string* outLogical) const{
		std::error_code relativeError;
		const std::filesystem::path relative = std::filesystem::relative(path, m_Model.Root, relativeError);
		if (relativeError || relative.empty())
			return false;
		if (outLogical)
			*outLogical = relative.generic_string();
		return true;
	}


Editor::TextureArtifactState ContentBrowserPanel::ResolveTextureBadge( const std::filesystem::path& sourcePath, bool* hasAsset){
		std::error_code stampError;
		const std::filesystem::file_time_type sourceStamp =
			std::filesystem::last_write_time(sourcePath, stampError);
		const std::filesystem::path assetPath = TextureAssetPathForSource(sourcePath.generic_string());
		// 产物 = `<同目录>/<源图主名>.wtexc`(契约名);旧命名 `<源图全名>.wtexc` 只作容错候选,
		// 与内核 TextureData 的查找顺序一致(.wtexc 不是资产 —— 浏览器里一律不显示)。
		std::filesystem::path artifactPath = sourcePath.parent_path()
			/ (sourcePath.stem().string() + ".wtexc");
		std::error_code artifactStampError;
		if (!std::filesystem::is_regular_file(artifactPath, artifactStampError))
			artifactPath = std::filesystem::path(sourcePath.string() + ".wtexc");
		const bool assetExists = std::filesystem::is_regular_file(assetPath, stampError);
		const std::filesystem::file_time_type assetStamp = assetExists
			? std::filesystem::last_write_time(assetPath, stampError)
			: std::filesystem::file_time_type {};
		const bool artifactExists = std::filesystem::is_regular_file(artifactPath, stampError);
		const std::filesystem::file_time_type artifactStamp = artifactExists
			? std::filesystem::last_write_time(artifactPath, stampError)
			: std::filesystem::file_time_type {};
		if (hasAsset)
			*hasAsset = assetExists;

		TextureBadgeEntry& entry = m_TextureBadges[sourcePath];
		if (entry.Valid && entry.HasAsset == assetExists && entry.HasArtifact == artifactExists
			&& entry.SourceStamp == sourceStamp && entry.AssetStamp == assetStamp
			&& entry.ArtifactStamp == artifactStamp)
			return entry.Artifact;

		std::string logical;
		if (!LogicalPathFor(sourcePath, &logical))
			logical = sourcePath.filename().generic_string();
		// 判定本身要读源字节算 sha256 —— 只在上面三项 mtime 真的变了时才走到这里。
		entry.Artifact = Editor::InspectTextureArtifact(m_Model.Root, logical, nullptr).State;
		entry.HasAsset = assetExists;
		entry.HasArtifact = artifactExists;
		entry.SourceStamp = sourceStamp;
		entry.AssetStamp = assetStamp;
		entry.ArtifactStamp = artifactStamp;
		entry.Valid = true;
		return entry.Artifact;
	}


void ContentBrowserPanel::RegisterTextureBadgeNode(const BrowserSlice& slice, const Wui::WuiRect& rect){
		std::string logical;
		if (!LogicalPathFor(slice.Path, &logical))
			logical = slice.Path.filename().generic_string();
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId(("browser.texture.badge." + logical).c_str());
		node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		node.Kind = "badge";
		node.Label = (slice.HasTextureAsset
			? Wui::Tr("panel.content_browser.badge.texture_asset", "Asset")
			: Wui::Tr("panel.content_browser.badge.texture_defaults", "Defaults"))
			+ " · " + TextureArtifactBadgeText(slice.TextureArtifact);
		node.Value = std::string("asset=") + (slice.HasTextureAsset ? "yes" : "no")
			+ ";artifact=" + TextureArtifactBadgeCode(slice.TextureArtifact);
		node.Tooltip = Wui::Tr("panel.content_browser.badge.texture_tooltip",
			"Source image of a texture: its import settings live in the sibling .wtex asset, and the "
			"status is the baked artifact (<stem>.wtexc).") + "  " + logical;
		node.Rect = rect;
		node.Enabled = true;
		node.Interactive = false;
		node.Visible = true;
		Wui::WuiAccessibility::Get().Register(node);
	}


	// CPPSRC-1:项目源码根下 `Generated/**` 的常驻徽标节点。网格/列表两种模式都登记同一个 id,
	// 脚本按 `browser.badge.generated.<相对源码根路径>` 就能断言"这枚文件标成了生成物"。
void ContentBrowserPanel::RegisterGeneratedBadgeNode(const BrowserSlice& slice, const Wui::WuiRect& rect){
		std::error_code relativeError;
		const std::filesystem::path relative =
			std::filesystem::relative(slice.Path, m_Model.Root, relativeError);
		const std::string logical = relativeError ? slice.Path.filename().generic_string()
			: relative.generic_string();
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId(("browser.badge.generated." + logical).c_str());
		node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		node.Kind = "badge";
		node.Label = Wui::Tr("asset.badge.generated", "Generated");
		node.Value = logical;
		node.Tooltip = Wui::Tr("panel.content_browser.badge.generated.tooltip",
			"Build-generated source (schema registration / manifest sync) — do not edit by hand.")
			+ "  " + logical;
		node.Rect = rect;
		node.Enabled = true;
		node.Interactive = false;
		node.Visible = true;
		Wui::WuiAccessibility::Get().Register(node);
	}


void ContentBrowserPanel::OpenTextureSettingsFor(const std::filesystem::path& path, bool resetConfirm){
		std::string logical;
		if (!LogicalPathFor(path, &logical))
		{
			NotifyAssetFailure(Wui::Tr("panel.content_browser.texture.outside_root",
				"Texture settings need an asset inside the content root."));
			return;
		}
		// 内容浏览器只写请求:可见性/布局切换在 EditorShell 的帧边界执行(见 TextureSettingsRequests)。
		Editor::TextureSettingsRequests::Get().Request(logical, resetConfirm
			? Editor::TextureSettingsRequests::Kind::ResetDefaults
			: Editor::TextureSettingsRequests::Kind::Open);
		if (m_Ctx)
			m_Ctx->RecordOp("browser", resetConfirm ? "texture-reset-ask" : "texture-settings", logical, "");
	}


void ContentBrowserPanel::CreateTextureAssetFor(const std::filesystem::path& sourcePath){
		std::string logical;
		if (!LogicalPathFor(sourcePath, &logical))
		{
			NotifyAssetFailure(Wui::Tr("panel.content_browser.texture.outside_root",
				"Texture settings need an asset inside the content root."));
			return;
		}
		std::string assetLogical;
		std::string error;
		if (!Editor::CreateTextureAssetForSource(m_Model.Root, logical, &assetLogical, error))
		{
			NotifyAssetFailure(error);
			WLD_CORE_WARN("[texture] create texture asset failed: {0}", error);
			return;
		}
		InvalidateContents();
		m_TextureBadges.erase(sourcePath);   // 徽标立刻重算("默认设置" → "有资产")
		m_Host.Notify(Wui::TrFormat("panel.content_browser.notice.texture_asset_created",
			"Created {asset}", { { "asset", assetLogical } }));
		WLD_CORE_INFO("[texture] created texture asset: {0}", assetLogical);
		const std::filesystem::path assetPath = m_Model.Root / assetLogical;
		SelectCreated(assetPath, "create-texture-asset");
		OpenTextureSettingsFor(assetPath, false);
	}


	// M4-TEX P5:把拖进来的图片**导入**到内容根(用户:「拖拽原始图片进入引擎应该执行导入功能」)。
	// 落点:光标在内容浏览器里 = 当前目录;拖到视口等其它区域 = 内容根默认 `textures/`。
	// 重名加 `-1`/`-2` 后缀(**不覆盖**已有文件);`.wtex` 写失败时把刚复制的文件一并删掉,不留半成品。
void ContentBrowserPanel::ImportDroppedTexture(const std::filesystem::path& source, const Wui::WuiRect& panelRect){
		std::filesystem::path destination = m_Model.Current;
		const bool overBrowser = m_Ctx != nullptr && m_Ctx->IsHovered(panelRect);
		if (!overBrowser || !IsWithinOrEqual(destination, m_Model.Root))
			destination = m_Model.Root / "textures";
		std::error_code directoryError;
		std::filesystem::create_directories(destination, directoryError);
		if (directoryError)
		{
			const std::string message = Wui::TrFormat("panel.content_browser.drop.texture_dir_failed",
				"Cannot create the import folder: {detail}",
				{ { "detail", directoryError.message() } });
			NotifyAssetFailure(message);
			WLD_CORE_WARN("[drop] {0}", message);
			return;
		}
		const std::filesystem::path target = MakeUniqueFileName(destination, source.filename().string());
		std::error_code copyError;
		std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, copyError);
		if (copyError)
		{
			const std::string message = Wui::TrFormat("panel.content_browser.drop.texture_copy_failed",
				"Cannot copy the dropped image: {detail}", { { "detail", copyError.message() } });
			NotifyAssetFailure(message);
			WLD_CORE_WARN("[drop] {0}", message);
			return;
		}
		std::string logical;
		if (!LogicalPathFor(target, &logical))
		{
			std::error_code cleanupError;
			std::filesystem::remove(target, cleanupError);
			NotifyAssetFailure(Wui::Tr("panel.content_browser.texture.outside_root",
				"Texture settings need an asset inside the content root."));
			return;
		}
		std::string assetLogical;
		std::string error;
		if (!Editor::EnsureTextureAssetForSource(m_Model.Root, logical, &assetLogical, nullptr, error))
		{
			// 资产写不出来 = 这次导入没有意义:把刚复制进来的文件也清掉(不留半成品)。
			std::error_code cleanupError;
			std::filesystem::remove(target, cleanupError);
			NotifyAssetFailure(error);
			WLD_CORE_WARN("[drop] texture import failed for '{0}': {1}", logical, error);
			return;
		}
		InvalidateContents();
		if (m_Model.Search[0])
			UpdateSearch();
		m_TextureBadges.erase(target);
		m_Host.Notify(Wui::TrFormat("panel.content_browser.notice.texture_imported",
			"Imported {source} (settings: {asset})",
			{ { "source", logical }, { "asset", assetLogical } }));
		WLD_CORE_INFO("[drop] imported texture '{0}' → '{1}' (settings {2})", source.string(), logical,
			assetLogical);
		if (m_Ctx)
			m_Ctx->RecordOp("browser", "drop-import-texture", source.filename().string(), logical);
		SelectAsset(logical, "import-texture");
	}


void ContentBrowserPanel::ReimportTextureAsset(const std::filesystem::path& assetPath){
		std::string logical;
		if (!LogicalPathFor(assetPath, &logical))
		{
			NotifyAssetFailure(Wui::Tr("panel.content_browser.texture.outside_root",
				"Texture settings need an asset inside the content root."));
			return;
		}
		// M4-TEX P9:`.wtex` 按资产文件读(容器 = 内嵌源字节;旧式 = 设置 + 外部源图),
		// 重烘走"字节来源"解析出来的逻辑路径(容器 = 资产自身)。
		const Editor::TextureAssetDocument document =
			Editor::LoadTextureAssetDocument(m_Model.Root, logical);
		std::string error;
		if (!document.Valid)
		{
			NotifyAssetFailure(document.Error);
			return;
		}
		Editor::TextureSourceResolution resolution;
		if (!Editor::ResolveTextureSource(m_Model.Root, logical, document.Settings, resolution)
			|| !Editor::BakeTextureArtifactNow(m_Model.Root, resolution.BytesLogical, document.Settings,
				error))
		{
			const std::string detail = resolution.Error.empty() ? error : resolution.Error;
			NotifyAssetFailure(detail);
			WLD_CORE_WARN("[texture] reimport '{0}' failed: {1}", logical, detail);
			return;
		}
		const std::string source = resolution.BytesLogical;
		m_TextureBadges.clear();   // 产物 mtime 变了;清表保证徽标立刻反映结果
		m_Host.Notify(Wui::TrFormat("panel.content_browser.notice.texture_reimported",
			"Re-baked {source}", { { "source", source } }));
		WLD_CORE_INFO("[texture] reimport ok: {0} (source {1})", logical, source);
	}


bool ContentBrowserPanel::GlobalCursorScreen(const Wui::WuiContext& ctx, float* outX, float* outY) const{
		if (!Application::HasInstance())
			return false;
		int windowX = 0, windowY = 0;
		Application::Get().GetWindow().GetPosition(&windowX, &windowY);
		const float scale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		const glm::vec2 mouse = ctx.Input().MousePos;
		if (outX)
			*outX = static_cast<float>(windowX) + mouse.x * scale;
		if (outY)
			*outY = static_cast<float>(windowY) + mouse.y * scale;
		return true;
	}


void ContentBrowserPanel::DeliverCrossWindowDrop(Wui::WuiContext& ctx){
		std::string payload;
		if (!ctx.IsDragActive(&payload) || payload.rfind("file:", 0) != 0)
			return;
		// 只在**释放那一帧**投递:EndFrame 才把拖拽收口,这里读到的还是"正在拖"的状态。
		if (!ctx.Input().MouseReleased[0])
			return;
		float screenX = 0.0f, screenY = 0.0f;
		if (!GlobalCursorScreen(ctx, &screenX, &screenY))
			return;
		// 命中别的窗口登记过的落点(材质编辑器的贴图槽 / 标题)才消费;
		// 命中不了 = 什么都不做 —— 浏览器自己的移动/落点逻辑照旧(下一帧 AcceptDrop)。
		if (Editor::AssetDropBridge::Get().DeliverFromScreen(screenX, screenY, payload))
		{
			if (m_Ctx)
				m_Ctx->RecordOp("browser", "drop-to-window", payload, "cross-window");
			// 这一次拖拽由别的窗口消费了:清掉浏览器自己的候选落点,免得它留到
			// **下一次**拖拽被 AcceptDrop 误当成落点(移动文件)。
			m_Model.PendingDropDest.clear();
			ctx.EndDrag();
		}
	}


void ContentBrowserPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		m_Ctx = &ctx;
		const Wui::WuiTheme& theme = host.Theme();
		// U25-M2:向导请求(注册表回调 / Window 菜单 / 材质面板的 Extract)在**下一帧的这里**落地 ——
		// 打开模态需要 WuiContext 与布局,只有渲染期才具备。
		if (m_NewMaterialPendingOpen)
		{
			m_NewMaterialPendingOpen = false;
			OpenNewMaterialModal(ctx, m_NewMaterialPendingFromSelection);
			m_NewMaterialPendingFromSelection = false;
		}
		// M4-S2/Slang-B1:`.slang` 新建向导同一套"下一帧落地"规则(注册表回调可能来自快捷键路径)。
		if (m_NewShaderPendingOpen)
		{
			m_NewShaderPendingOpen = false;
			OpenNewShaderModal(ctx);
			m_NewShaderPendingDir.clear();
		}
		// U25-M2:跨窗口拖放(内容浏览器 → 材质编辑器的贴图槽/标题):在**释放那一帧**把
		// "file:<逻辑路径>" 交给登记过落点的面板(核心 WUI 的拖拽态是每窗口一份,见 AssetDropBridge)。
		DeliverCrossWindowDrop(ctx);
		// P4-U13d:选中状态进无障碍树 —— 创建资产(预制体等)之后,脚本/读屏要能确认
		// "内容浏览器真的选中了哪个资产"。此前选中只画在画面上,ui.tree 读不到。
		{
			std::string selectedText;
			if (!m_Model.LastSelected.empty())
			{
				std::error_code selectionError;
				const std::filesystem::path relative =
					std::filesystem::relative(m_Model.LastSelected, m_Model.Root, selectionError);
				selectedText = selectionError ? m_Model.LastSelected.generic_string()
					: relative.generic_string();
			}
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("browser.selection");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.content_browser.selection", "Selected asset");
			node.Value = selectedText;   // 逻辑路径;空 = 当前没有选中
			node.Tooltip = Wui::Tr("panel.content_browser.selection.tooltip",
				"Logical path of the selected asset (empty = nothing selected)");
			node.Rect = rect;
			node.Enabled = true;
			node.Interactive = false;
			Wui::WuiAccessibility::Get().Register(node);
		}
		// D10:OS 文件拖放(资源管理器 → 窗口)。平台层把拖入路径记在**收到拖放的窗口**上,
		// 这里消费主窗口的队列:`.gltf/.glb` → 导入到**当前文件夹**(用户 Q1/Q2);
		// 其它类型明确提示"不支持该类型",不静默丢弃。
		{
			std::vector<std::string> dropped = Application::Get().GetWindow().ConsumeDroppedFiles();
			AppendInjectedDroppedFiles(dropped);   // 探针钩子(见 helper 注释;真实链路不变)
			for (const std::string& droppedPath : dropped)
			{
				const std::filesystem::path source(droppedPath);
				const std::string extension = LowerExtension(source);
				if (extension == ".gltf" || extension == ".glb")
				{
					std::error_code destError;
					const std::filesystem::path destRelative =
						std::filesystem::relative(m_Model.Current, m_Model.Root, destError);
					const std::string destination =
						destError ? std::string() : destRelative.generic_string();
					std::string message;
					std::string logicalModel;
					if (!m_Host.ImportModelFileTo(source.string(), destination, &message, &logicalModel))
					{
						WLD_CORE_WARN("[drop] 导入 '{0}' 失败: {1}", droppedPath, message);
						if (m_Ctx) m_Ctx->RecordOp("browser", "drop-import-failed",
							source.filename().string(), message);
					}
					else
					{
						WLD_CORE_INFO("[drop] {0}", message);
						if (m_Ctx) m_Ctx->RecordOp("browser", "drop-import",
							source.filename().string(), message);
						InvalidateContents();
						if (m_Model.Search[0])
							UpdateSearch();
						if (!logicalModel.empty())
						{
							SelectCreated(m_Model.Root / logicalModel, "import-model");
							m_Host.OpenModelPreview(logicalModel);
						}
					}
				}
				else if (IsTextureSourcePath(source))
				{
					// M4-TEX P5(用户 2026-09-25:「拖拽原始图片进入引擎应该执行导入功能」):
					// 图片从资源管理器拖进来 = 导入 → 复制进内容根 + 写最小 `.wtex` + 选中。
					ImportDroppedTexture(source, rect);
				}
				else
				{
					// Q2:非 glTF 类型不支持(不复制、不静默)。
					const std::string message = Wui::Tr("panel.content_browser.drop.unsupported",
						"Unsupported file type: ") + extension
						+ Wui::Tr("panel.content_browser.drop.unsupported_hint",
							"(drop .gltf / .glb to import a model, or .png / .jpg / .jpeg / .tga / .bmp "
							"to import a texture)");
					WLD_CORE_WARN("[drop] {0}", message);
					if (m_Ctx) m_Ctx->RecordOp("browser", "drop-rejected",
						source.filename().string(), message);
				}
			}
		}
		// 窗口/GL 上下文重建后旧图标纹理失效:丢弃缓存,重新加载并注册。
		if (m_TextureEpoch != host.TextureEpoch())
		{
			m_TextureEpoch = host.TextureEpoch();
			m_DirIcon = nullptr;
			m_FileIcon = nullptr;
			m_DirIconId = 0;
			m_FileIconId = 0;
		}
		// VEC-H6:编辑器图标资产 PNG → `.wtex` 单文件容器(payload = 原 PNG 字节,外观不变)。
		if (!m_DirIcon)
			m_DirIcon = TextureLibrary::Get().Get(EditorResourcePath("assets/icons/ContentBrowser/DirectoryIcon.wtex"), /*srgb*/ true);
		if (!m_FileIcon)
			m_FileIcon = TextureLibrary::Get().Get(EditorResourcePath("assets/icons/ContentBrowser/FileIcon.wtex"), /*srgb*/ true);
		Wui::WuiTextureRegistry& registry = Wui::WuiTextureRegistry::Get();
		if (registry.Generation() != m_IconGeneration)
		{
			m_DirIconId = registry.Register(m_DirIcon);
			m_FileIconId = registry.Register(m_FileIcon);
			m_IconGeneration = registry.Generation();
		}

		std::string filePayload;
		const bool fileDrag = ctx.IsDragActive(&filePayload) && filePayload.rfind("file:", 0) == 0;

		// ---- 工具栏(P4-UX14 重新设计)----
		// 用户:"顶部那排按钮真的有必要存在吗?" —— 结论:**只留三样**。
		//   ① 面包屑(路径本身就是导航,点任意一级直接跳);
		//   ② 搜索(常驻,高频);
		//   ③ 一个 `⋯` 菜单(新建/刷新/视图切换/在资源管理器中打开/后退前进上一级)。
		// 为什么删掉那六个动作按钮:后退/前进/上一级在树和面包屑里都有等价入口,
		// 新建/刷新/视图是低频动作 —— 六个常驻按钮换一个菜单,工具栏从"一排控件"回到"一行路径",
		// 与 VS Code 资源管理器/Blender 浏览器一致(导航靠树,动作靠菜单 + 快捷键)。
		if (!m_Toolbar)
		{
			m_Toolbar = std::make_shared<Wui::WuiBox>();
			m_Toolbar->Direction = Wui::WuiDirection::Row;
			m_Toolbar->Gap = 6;
			m_Toolbar->AlignCross = Wui::WuiAlign::Center;

			auto addButton = [&](const std::string& label, std::function<void()> action, float width,
				bool centerLabel = false)
			{
				auto button = std::make_shared<Wui::WuiButton>();
				button->Label = label;
				button->CenterLabel = centerLabel;
				button->OnClick = std::move(action);
				m_Toolbar->Add(button, { width, width, 0, 24, 0 });
				return button;
			};
			m_Breadcrumbs = std::make_shared<Wui::WuiBox>();
			m_Breadcrumbs->Direction = Wui::WuiDirection::Row;
			m_Breadcrumbs->Gap = 4;
			m_Breadcrumbs->AlignCross = Wui::WuiAlign::Center;
			// P4-UX13:面包屑**吃掉剩余宽度**(以前靠 spacer 顶到右边),这样面板变窄时先挤的是路径,
			// 而不是把右侧的搜索框/动作挤出画面(实测 890px 宽时搜索框整个看不见)。
			// P4-UX15:面包屑**不再进布局树**(嵌套 Box 的宽度预算在窄面板/深路径下不可控,
			// 实测路径会整条消失)。这里只放一个占位 spacer,真正的面包屑在工具栏画完后
			// 用立即模式逐枚画(见下方"面包屑(立即模式)")。
			auto toolbarSpacer = std::make_shared<Wui::WuiSpacer>();
			m_Toolbar->Add(toolbarSpacer, { 8, 1e30f, 0, 24, 1 });

			// `⋯` 菜单:动作收进一处,低频动作不再占用常驻空间。
			// "…"(U+2026)在字体子集里有;"⋯"(U+22EF)没有 → 会画成乱码(用户实测)。
			m_MoreButton = addButton("…", [this] { m_ToolbarMenuOpen = true; }, 30, true);
			m_MoreButton->CenterLabel = true;

			m_SearchField = std::make_shared<Wui::WuiTextField>();
			// D10:给搜索框一个稳定 id —— AI/脚本可以 ui.type 驱动它(以前只能手点)。
			m_SearchField->SetId(Wui::HashId("browser.search"));
			// P4-U5a:占位提示是面板自己画的 Label,读屏/脚本读不到 → 显式喂给控件
			// (节点 label=控件名,value=空输入时的占位文案)。
			m_SearchField->A11yLabel = Wui::Tr("panel.content_browser.search.a11y", "Search assets");
			m_SearchField->A11yPlaceholder = Wui::Tr("panel.content_browser.search.hint", "Search assets…");
			m_SearchField->Buffer = &m_Model.SearchEdit;
			m_SearchField->OnCommit = [this]
				{
					std::strncpy(m_Model.Search, m_Model.SearchEdit.c_str(), sizeof(m_Model.Search) - 1);
					m_Model.Search[sizeof(m_Model.Search) - 1] = 0;
					UpdateSearch();
				};
			m_Toolbar->Add(m_SearchField, { 140, 162, 0, 24, 0 });
		}

		// 导航快捷键(工具栏删掉三个导航按钮后的补偿):Alt+←/→ 后退/前进、Alt+↑ 上一级。
		// 文本控件持焦点时不抢(否则在搜索框里按方向键会跳目录)。
		if (!Wui::WuiTextFocus::Get().Active() && ctx.Input().Alt)
		{
			if (ctx.WasKeyPressed(KeyCodes::Left) && m_Model.HistoryIndex > 0)
				GoBack();
			else if (ctx.WasKeyPressed(KeyCodes::Right)
				&& m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) - 1)
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
			else if (ctx.WasKeyPressed(KeyCodes::Up) && m_Model.Current != m_Model.Root)
				GoUp();
		}

		// 面包屑随路径变化重建。
		if (m_LastCrumbPath != m_Model.Current)
		{
			m_LastCrumbPath = m_Model.Current;
			m_CrumbButtons.clear();
			m_CrumbDests.clear();
			m_CrumbLabels.clear();
			m_Breadcrumbs->Clear();
			const auto addCrumb = [&](const std::string& label, const std::filesystem::path& destination)
			{
				auto button = std::make_shared<Wui::WuiButton>();
				button->Label = label;
				button->OnClick = [this, destination] { Navigate(destination); };
				m_Breadcrumbs->Add(button, { static_cast<float>(label.size() * 8 + 20), static_cast<float>(label.size() * 8 + 20), 0, 24, 0 });
				m_CrumbButtons.push_back(button);
				m_CrumbDests.push_back(destination);
				m_CrumbLabels.push_back(label);
			};
			addCrumb(Wui::Tr("panel.content_browser.root", "Root"), m_Model.Root);
			// 面包屑用 › 分隔而不是一串等距按钮:路径的"层级感"来自分隔符与当前段的高亮。
			const auto addCrumbSeparator = [&]()
			{
				auto separator = std::make_shared<Wui::WuiLabel>();
				separator->Text = "›";
				separator->Color = theme.TextMuted;
				separator->FontSize = 13.0f;
				separator->FixedWidth = 12.0f;
				m_Breadcrumbs->Add(separator, { 12.0f, 12.0f, 0, 24, 0 });
			};
			std::filesystem::path accumulated = m_Model.Root;
			// 路径太深时只显示最后两级 + "...":整条路径铺不下会把面包屑挤爆(面板一窄就"消失")。
			std::vector<std::string> parts;
			for (const auto& part : m_Model.Current.lexically_relative(m_Model.Root))
			{
				// 根目录自身的相对路径是 ".":它不是一个真实层级,别画成一枚空的 crumb
				// (实测工具条最右侧出现一个 "." 按钮,看起来像坏掉的控件)。
				if (part == "." || part.empty())
					continue;
				parts.push_back(part.string());
			}
			const size_t firstShown = parts.size() > 3 ? parts.size() - 2 : 0;
			if (firstShown > 0)
			{
				// 前面被折叠的部分不再逐级列出,直接给一个不可点的省略号占位。
				auto dots = std::make_shared<Wui::WuiLabel>();
				dots->Text = "…";
				dots->Color = theme.TextMuted;
				dots->FontSize = 13.0f;
				dots->FixedWidth = 14.0f;
				m_Breadcrumbs->Add(dots, { 14.0f, 14.0f, 0, 24, 0 });
				for (size_t i = 0; i < firstShown; ++i)
					accumulated /= parts[i];
			}
			for (size_t i = firstShown; i < parts.size(); ++i)
			{
				accumulated /= parts[i];
				addCrumbSeparator();
				addCrumb(parts[i], accumulated);
			}
		}

		Wui::LayoutWidgetTree(m_Toolbar, { rect.X + 6, rect.Y + 6, rect.W - 12, 24 });
		Wui::WuiPaintContext toolbarPaint(ctx);
		m_Toolbar->Paint(toolbarPaint);
		// ---- 面包屑(立即模式)----
		// P4-UX15:不再依赖嵌套 Box —— 显式按 x 预算排布,放不下的中间层级收成 "…",
		// 保证"进入任意深度的文件夹,路径都看得见"(用户实测:以前一进目录整条路径消失)。
		{
			const float budgetRight = m_SearchField->Rect().X - 12.0f;
			float x = rect.X + 6.0f;
			for (size_t i = 0; i < m_CrumbLabels.size(); ++i)
			{
				const std::string& label = m_CrumbLabels[i];
				const bool last = (i + 1) == m_CrumbLabels.size();
				const float width = std::min(180.0f, ctx.MeasureTextWidth(label, 13.0f) + 18.0f);
				if (!last && x + width > budgetRight)
				{
					Wui::Label(ctx, { x, rect.Y + 11.0f }, "…", theme.TextMuted, 12.0f);
					x += 16.0f;
					continue;
				}
				const Wui::WuiRect crumb { x, rect.Y + 6.0f, width, 24.0f };
				const Wui::WuiId crumbId = Wui::HashId(("browser.crumb." + std::to_string(i)).c_str());
				if (Wui::Button(ctx, crumbId, crumb, label, theme) && !last)
					Navigate(m_CrumbDests[i]);
				Wui::WuiAccessNode node;
				node.Id = crumbId;
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "crumb";
				node.Label = label;
				node.Value = last ? "current" : "parent";
				node.Rect = crumb;
				node.Enabled = true;
				node.Interactive = !last;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
				if (!last)
					Wui::Label(ctx, { crumb.X + crumb.W + 2.0f, rect.Y + 11.0f }, "›", theme.TextMuted, 12.0f);
				x += width + 14.0f;
			}
		}

		// P4-UX13:工具条 = 四组(导航 | 路径 | 搜索 | 视图与动作),组间画 1px 分隔线;
		// 每个图标按钮都登记悬停说明 —— 图标没有 tooltip 就等于没解释。
		{
			const auto separator = [&](const Wui::WuiRect& firstOfGroup)
			{
				// 缺件(W3.2):库里没有"即时分隔线"(WuiSeparator 是占位 widget、ToolbarSeparator 几何不同),
				// 这一条按纯填充走 PanelBackground —— 命令逐字段不变,语义缺口进报告 §4-3。
				Wui::PanelBackground(ctx, { firstOfGroup.X - 8.0f, rect.Y + 9.0f, 1.0f, 18.0f },
					theme.Border, 0.0f);
			};
			separator(m_SearchField->Rect());
			if (m_MoreButton)
				separator(m_MoreButton->Rect());

			if (m_MoreButton)
				Wui::Tooltip(ctx, m_MoreButton->Rect(), Wui::Tr("panel.content_browser.more.tooltip",
					"More actions: New / Refresh / View / Back-Forward (Alt+←/→/↑)"));
			// P4-UX16:`…` 是工具条上唯一的"动作入口",但对象式 WuiButton 不进无障碍树 ——
			// 结果脚本只能靠猜坐标点它(实测:布局一变就点空)。这里按面包屑同款做法补一个稳定节点,
			// `ui.invoke id=browser.more` 就能打开菜单(与真实点击同一条输入路径)。
			if (m_MoreButton)
			{
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId("browser.more");
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "button";
				node.Label = "…";
				node.Value = ctx.IsPopupOpen(Wui::HashId("browser.toolbar.menu")) ? "open" : "closed";
				node.Tooltip = Wui::Tr("panel.content_browser.more.tooltip",
					"More actions: New / Refresh / View / Back-Forward (Alt+←/→/↑)");
				node.Rect = m_MoreButton->Rect();
				Wui::WuiAccessibility::Get().Register(node);
			}
			// 搜索框:空时给占位提示,有内容时右侧给"清除"(与浏览器/编辑器的搜索框一致)。
			const Wui::WuiRect searchRect = m_SearchField->Rect();
			if (m_Model.SearchEdit.empty())
				Wui::Label(ctx, { searchRect.X + 8.0f, searchRect.Y + 5.0f },
					Wui::Tr("panel.content_browser.search.hint", "Search assets…"), theme.TextDisabled, 12.0f);
			else
			{
				const Wui::WuiRect clearRect { searchRect.X + searchRect.W - 20.0f, searchRect.Y + 2.0f, 18.0f, 20.0f };
				if (Wui::Button(ctx, Wui::HashId("browser.search.clearfield"), clearRect, "x", theme))
				{
					m_Model.SearchEdit.clear();
					m_Model.Search[0] = 0;
					UpdateSearch();
				}
				Wui::Tooltip(ctx, clearRect, Wui::Tr("panel.content_browser.search.clear.tooltip", "Clear search"));
			}
		}

		if (fileDrag)
			for (size_t i = 0; i < m_CrumbButtons.size(); ++i)
				if (ctx.IsHovered(m_CrumbButtons[i]->Rect()))
				{
					// PLUG-T3:项目插件根 = 只读浏览 —— 拖放导入/移动整条锁死。
					if (m_Model.Scope == BrowserRootScope::ProjectPlugins)
						break;
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
		// ---- CPPSRC-1 + PLUG-T3:三行"根"(内容根 ⇄ 项目 C++ 源码根 ⇄ 项目插件根)----
		// 用户 2026-09-29「c++脚本要像 asset 资产一样在编辑器里展示」:项目 C++ 从 Scripts 面板的
		// 列表搬到这里,和资产共用同一套网格/列表/类型列/搜索。不可用(没项目 / 没有 <项目根>/src)时
		// 第二行灰显并把理由写进 tooltip —— 灰按钮不能没有理由。
		// 用户 2026-09-30「对于项目的插件,也应该要像 c++ 代码一样可以在引擎中看到」:第三行同款。
		const float kRootRowH = 22.0f;
		const bool sourcesAvailable = ProjectSourcesAvailable();
		const bool pluginsAvailable = ProjectPluginsAvailable();
		const float treeListTop = treeRect.Y + kRootRowH * 3.0f + 1.0f;
		const Wui::WuiRect treeListRect { treeRect.X, treeListTop, treeRect.W,
			std::max(0.0f, treeRect.H - (treeListTop - treeRect.Y)) };
		{
			const Wui::WuiRect contentRow { treeRect.X, treeRect.Y, treeRect.W, kRootRowH };
			const Wui::WuiRect sourcesRow { treeRect.X, treeRect.Y + kRootRowH, treeRect.W, kRootRowH };
			const Wui::WuiRect pluginsRow { treeRect.X, treeRect.Y + kRootRowH * 2.0f, treeRect.W, kRootRowH };
			const std::string contentLabel = Wui::Tr("panel.content_browser.root.content", "Content");
			const std::string sourcesLabel = Wui::Tr("panel.content_browser.root.project_sources", "Project C++");
			const std::string pluginsLabel = Wui::Tr("panel.content_browser.root.project_plugins", "Project Plugins");
			const std::string contentTooltip = Wui::Tr("panel.content_browser.root.content.tooltip",
				"Content root: <project>/assets — scenes, materials, textures, prefabs, scripts.");
			const std::string sourcesTooltip = sourcesAvailable
				? Wui::Tr("panel.content_browser.root.project_sources.tooltip",
					"Project C++ (<project>/src): same grid/list, type column and search as assets; "
					"double-click a file to open it in Visual Studio.")
				: Wui::Tr("panel.content_browser.src.root_unavailable",
					"Open or create a project first — project C++ lives in <project>/src.");
			const std::string pluginsTooltip = pluginsAvailable
				? Wui::Tr("panel.content_browser.root.project_plugins.tooltip",
					"Project plugins (<project>/plugins): same grid/list, type column and search as assets; "
					"read-only — double-click a C++ file or plugin.we.yaml to open it in Visual Studio.")
				: Wui::Tr("panel.content_browser.plugins.root_unavailable",
					"Open a project with a plugins/ directory first — project plugins live in <project>/plugins.");
			const auto rootRow = [&](const Wui::WuiRect& row, BrowserRootScope scope, const char* idText,
				const std::string& label, const std::string& tooltip, const std::filesystem::path& root,
				bool enabled) -> bool
			{
				const bool active = m_Model.Scope == scope;
				const bool hovered = enabled && ctx.IsHovered(row);
				Wui::PanelBackground(ctx, row,
					active ? theme.ButtonHover : (hovered ? theme.ButtonBg : theme.PanelHeader), 3.0f);
				if (active)
					Wui::HighlightOutline(ctx, row, theme.Accent, 3.0f, 1.0f);
				Wui::Label(ctx, { row.X + 20.0f, row.Y + (row.H - 13.0f) * 0.5f }, label,
					enabled ? (active ? theme.Accent : theme.Text) : theme.TextDisabled, 13.0f);
				Wui::WuiAccessNode node;
				node.Id = Wui::HashId(idText);
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "list-item";
				node.Label = label;
				node.Value = root.generic_string();
				node.Tooltip = tooltip;
				node.Rect = row;
				node.Enabled = enabled;
				node.Focused = active;
				node.Interactive = true;
				node.Visible = true;
				Wui::WuiAccessibility::Get().Register(node);
				ctx.RegisterFocusable(node.Id, row);
				Wui::DrawFocusRing(ctx, row, node.Id, theme);
				if (hovered)
				{
					if (enabled)
						ctx.SetCursor(Wui::WuiCursor::Hand);
					if (!tooltip.empty())
						ctx.SetTooltip(tooltip);
				}
				if (!enabled)
					return false;
				// 鼠标点击与键盘 Enter/Space 走同一条切换路径(焦点在根行上时)。
				const bool activated = ctx.IsClicked(row)
					|| (ctx.Focus() == node.Id
						&& (ctx.WasKeyTriggered(KeyCodes::Enter) || ctx.WasKeyTriggered(KeyCodes::Space)));
				return activated;
			};
			if (rootRow(contentRow, BrowserRootScope::Content, "browser.root.content", contentLabel,
					contentTooltip, m_Model.ContentRoot, true))
				SwitchRoot(BrowserRootScope::Content);
			if (rootRow(sourcesRow, BrowserRootScope::ProjectSources, "browser.root.project-sources",
					sourcesLabel, sourcesTooltip, ProjectSourceRoot(), sourcesAvailable))
				SwitchRoot(BrowserRootScope::ProjectSources);
			if (rootRow(pluginsRow, BrowserRootScope::ProjectPlugins, "browser.root.project-plugins",
					pluginsLabel, pluginsTooltip, ProjectPluginsRoot(), pluginsAvailable))
				SwitchRoot(BrowserRootScope::ProjectPlugins);
			// 根行与目录树之间的分隔线(走库件 `Wui::PanelBackground` 的单色填充,不新增裸绘制)。
			Wui::PanelBackground(ctx, { treeRect.X + 8.0f, treeListTop - 1.0f, treeRect.W - 16.0f, 1.0f },
				theme.Border, 0.0f);
		}
		// 目录树走 TreeView 组件:展开箭头/悬停/选中由组件绘制,
		// 导航、拖拽起手、拖入目标仍由面板处理(用组件返回的 ItemRects)。
		std::vector<const BrowserDirNode*> visibleNodes;
		std::vector<Wui::TreeViewItem> treeItems;
		for (const BrowserDirNode& node : m_Model.DirTree)
		{
			// CPPSRC-1 + PLUG-T3:根由上面三行"根行"代表,树里不再重复画同一个根行。
			if (node.Depth == 0)
				continue;
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
		bool treeRenameDrawn = false;
		{
			// P4-UX14:传 id 之后树才有键盘(Tab 停在这里时 ↑↓←→/Enter 生效)。
			Wui::TreeViewResult tree = Wui::TreeView(ctx, treeListRect, treeItems, 22.0f, m_Model.TreeScroll, theme,
				Wui::HashId("browser.tree"));
			// 键盘导航:控件只报"想做什么",改模型仍走与鼠标同一条路径。
			if (tree.KeyMoveTo >= 0 && tree.KeyMoveTo < static_cast<int>(visibleNodes.size()))
			{
				Navigate(visibleNodes[tree.KeyMoveTo]->Path);
				m_Model.TreeScroll = std::max(0.0f, 22.0f * static_cast<float>(tree.KeyMoveTo) - treeListRect.H * 0.5f);
			}
			else if (tree.KeyToggleExpand >= 0 && tree.KeyToggleExpand < static_cast<int>(visibleNodes.size()))
			{
				const std::filesystem::path& path = visibleNodes[tree.KeyToggleExpand]->Path;
				// 固定根不参与展开/折叠(控件本来也只为 HasChildren 行发这个键)。
				if (path != m_Model.Root)
				{
					if (treeItems[tree.KeyToggleExpand].Expanded)
						m_Model.TreeOpen.erase(path);
					else
						m_Model.TreeOpen.insert(path);
					SaveState();
				}
			}
			else if (tree.KeyActivate >= 0 && tree.KeyActivate < static_cast<int>(visibleNodes.size()))
			{
				Navigate(visibleNodes[tree.KeyActivate]->Path);
			}
			// D10-6:树行右键 → 打开树菜单(命中由 TreeView 的 ContextClicked 提供,不自己写命中检测)。
			if (tree.ContextClicked >= 0 && tree.ContextClicked < static_cast<int>(visibleNodes.size()))
			{
				// 与内容区菜单互斥:开新菜单前关掉旧菜单,避免两个菜单叠在一起。
				ctx.CloseAllPopups();
				m_TreeMenuPath = visibleNodes[tree.ContextClicked]->Path;
				m_TreeMenuPos = ctx.Input().MousePos;
				ctx.OpenPopup(Wui::HashId("browser.tree.context"));
			}
			for (size_t i = 0; i < visibleNodes.size(); ++i)
			{
				const BrowserDirNode& node = *visibleNodes[i];
				if (i >= tree.ItemRects.size())
					break;
				const Wui::WuiRect& row = tree.ItemRects[i];
				const bool hovered = ctx.IsHovered(row);
				if (tree.ClickedArrow == static_cast<int>(i))
				{
					if (node.Depth > 0)   // 固定根没有箭头,这里只是防御性判断
					{
						if (treeItems[i].Expanded) m_Model.TreeOpen.erase(node.Path);
						else m_Model.TreeOpen.insert(node.Path);
						SaveState();
					}
				}
				else if (tree.Clicked == static_cast<int>(i))
				{
					// 树菜单发起的重命名:点击落在该行输入框上时不要顺带导航进这个目录。
					if (!(m_Model.RenameActive && m_Model.RenameTarget == node.Path && m_TreeRenameTarget == node.Path))
						Navigate(node.Path);
				}
				// D10-6:从树菜单发起的重命名,输入框画在树行上(内容区那一份跳过,避免同一 id 画两份)。
				if (!treeRenameDrawn && m_Model.RenameTarget == node.Path && m_TreeRenameTarget == node.Path
					&& !(row.Y + row.H < treeListRect.Y || row.Y > treeListRect.Y + treeListRect.H))
				{
					RenderRenameField(ctx, node.Path,
						{ row.X + 18.0f, row.Y + 1.0f, std::max(60.0f, row.W - 22.0f), row.H - 2.0f }, theme);
					treeRenameDrawn = true;
				}
				if (ctx.Input().MouseDown[0] && hovered && node.Path != m_Model.Root
					&& m_Model.Scope != BrowserRootScope::ProjectPlugins)
				{
					const std::filesystem::path rel = node.Path.lexically_relative(m_Model.Root);
					ctx.BeginDrag(Wui::HashId(("browser.drag." + rel.string()).c_str()), "file:" + rel.string());
					ctx.SetCursor(Wui::WuiCursor::Hand);
				}
				if (fileDrag && hovered)
				{
					if (m_Model.Scope != BrowserRootScope::ProjectPlugins)
					{
						ctx.DropTarget(row, "file:");
						m_Model.PendingDropDest = node.Path;
						Wui::HighlightOutline(ctx, row, theme.Accent, 2.0f, 2.0f);
					}
				}
			}
		}

		// ---- 树行右键菜单(D10-6:基础操作;每个菜单项带稳定无障碍 id,AI 可点) ----
		// ---- `⋯` 工具栏菜单(P4-UX14):新建/刷新/视图切换/打开/导航都收在这里 ----
		const Wui::WuiId toolbarPopup = Wui::HashId("browser.toolbar.menu");
		if (m_ToolbarMenuOpen)
		{
			m_ToolbarMenuOpen = false;
			ctx.CloseAllPopups();
			m_ToolbarMenuPos = m_MoreButton
				? glm::vec2 { m_MoreButton->Rect().X, m_MoreButton->Rect().Y + m_MoreButton->Rect().H + 2.0f }
				: glm::vec2 { rect.X + 6.0f, rect.Y + 32.0f };
			ctx.OpenPopup(toolbarPopup);
		}
		if (ctx.IsPopupOpen(toolbarPopup))
		{
			const float menuW = 236.0f;
			const float itemH = 22.0f;
			// P4-UX16:新建从"两条硬编码项"变成一行 "New ▶" + 注册表驱动的子菜单 → 项数 8 → 7。
			const int itemCount = 7;
			const Wui::WuiRect menuRect { m_ToolbarMenuPos.x, m_ToolbarMenuPos.y, menuW,
				itemH * static_cast<float>(itemCount) + 8.0f };
			const auto goForward = [this]
			{
				if (m_Model.HistoryIndex >= static_cast<int>(m_Model.History.size()) - 1)
					return;
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
			};
			ctx.PushOverlay();
			Wui::DrawPanelSurface(ctx, menuRect, theme);
			// P4-U7:登记为覆盖层矩形 → 下一帧只挡下层控件(树/切片不再吃掉菜单上的点击)。
			ctx.RegisterOverlayRect(menuRect);
			int hoveredIndex = -1;
			auto item = [&](int index, const std::string& label, bool enabled, std::function<void()> action)
			{
				const Wui::WuiRect row { menuRect.X + 4.0f,
					menuRect.Y + 4.0f + itemH * static_cast<float>(index), menuW - 8.0f, itemH };
				if (enabled && ctx.IsHovered(row))
					hoveredIndex = index;
				const Wui::WuiId itemId = Wui::HashId(("browser.toolbar.menu." + std::to_string(index)).c_str());
				if (Wui::MenuItem(ctx, itemId, row, label, enabled, theme) && enabled)
				{
					action();
					ctx.ClosePopup(toolbarPopup);
				}
			};
			// 0 = 新建(展开的清单 = 资产类型注册表,不再硬编码类型)
			const Wui::WuiRect newRow { menuRect.X + 4.0f, menuRect.Y + 4.0f, menuW - 8.0f, itemH };
			const bool toolbarSourcesScope = m_Model.Scope == BrowserRootScope::ProjectSources;
			const bool toolbarPluginsScope = m_Model.Scope == BrowserRootScope::ProjectPlugins;
			const std::string readOnlyLockedReason = Wui::Tr("panel.content_browser.src.locked_reason",
				"C++ sources are built by CMake — rename or delete them in Visual Studio or File Explorer.");
			if (toolbarSourcesScope)
			{
				// CPPSRC-1:源码根下"新建"只有 C++ 组件一条(材质/场景/脚本/文件夹都是内容根语义);
				// 走宿主同一个向导(File ▸ 新建 C++ 组件…),不再展开资产类型清单。
				const std::string newCppLabel = Wui::Tr("panel.content_browser.toolbar.new_cpp", "New C++ …");
				if (Wui::MenuItem(ctx, Wui::HashId("browser.toolbar.menu.newcpp"), newRow, newCppLabel, true, theme))
				{
					m_Host.RequestNewCppScript();
					ctx.ClosePopup(toolbarPopup);
				}
			}
			else if (toolbarPluginsScope)
			{
				// PLUG-T3:项目插件根 = 只读浏览 —— "新建"整条锁死(理由与项目 C++ 根共用同一条)。
				const std::string newLockedLabel = Wui::Tr("panel.content_browser.toolbar.new_locked", "New…");
				const Wui::WuiId newLockedId = Wui::HashId("browser.toolbar.menu.newlocked");
				if (!Wui::MenuItem(ctx, newLockedId, newRow, newLockedLabel, false, theme))
					RegisterDisabledMenuItem(ctx, newLockedId, newRow, newLockedLabel, readOnlyLockedReason);
			}
			else if (RenderNewAssetRow(ctx, Wui::HashId("browser.toolbar.menu.0"), newRow, theme))
				m_NewMenuOwner = (m_NewMenuOwner == 1) ? 0 : 1;
			if (ctx.IsHovered(newRow))
				hoveredIndex = 0;
			item(1, Wui::Tr("panel.content_browser.refresh", "Refresh"), true, [this]
				{
					InvalidateContents();
					if (m_Model.Search[0])
						UpdateSearch();
				});
			item(2, m_Model.ListMode
					? Wui::Tr("panel.content_browser.menu.to_grid", "View: Switch to Grid")
					: Wui::Tr("panel.content_browser.menu.to_list", "View: Switch to List"),
				true, [this] { m_Model.ListMode = !m_Model.ListMode; SaveState(); });
			item(3, Wui::Tr("panel.content_browser.menu.reveal", "Open in Explorer"),
				m_Model.Current != m_Model.Root, [this] { OpenFolderInExplorer(m_Model.Current); });
			item(4, Wui::Tr("panel.content_browser.menu.back", "Back") + "   (Alt+←)",
				m_Model.HistoryIndex > 0, [this] { GoBack(); });
			item(5, Wui::Tr("panel.content_browser.menu.forward", "Forward") + "   (Alt+→)",
				m_Model.HistoryIndex < static_cast<int>(m_Model.History.size()) - 1, goForward);
			item(6, Wui::Tr("panel.content_browser.menu.up", "Up") + "   (Alt+↑)",
				m_Model.Current != m_Model.Root, [this] { GoUp(); });
			// 展开的"新建"清单(右/左侧贴边翻转);idx>0 的行被悬停 = 用户离开了新建行 → 收起。
			Wui::WuiRect newMenuRect;
			if (m_NewMenuOwner == 1 && !toolbarSourcesScope && !toolbarPluginsScope)
				newMenuRect = RenderNewAssetItems(ctx, "browser.toolbar.menu.new.", menuRect, rect, theme);
			ctx.PopOverlay();
			if (m_NewMenuOwner == 1 && hoveredIndex > 0)
				m_NewMenuOwner = 0;
			// Esc:先收子菜单,再收父菜单(与右键菜单一致)。
			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				if (m_NewMenuOwner == 1)
					m_NewMenuOwner = 0;
				else
					ctx.ClosePopup(toolbarPopup);
			}
			// P4-UX15:点面板里其它任何地方都收起 `…` 菜单(用户实测:以前点外面不关)。
			// P4-UX16:展开的"新建"清单与父菜单算**同一块**点击区 —— 否则点子菜单里的项会
			// 被当成"点到了外面"而先把父菜单关掉。
			Wui::WuiRect clickBlock = menuRect;
			if (newMenuRect.W > 0.0f)
			{
				const float right = std::max(menuRect.X + menuRect.W, newMenuRect.X + newMenuRect.W);
				const float bottom = std::max(menuRect.Y + menuRect.H, newMenuRect.Y + newMenuRect.H);
				clickBlock.X = std::min(clickBlock.X, newMenuRect.X);
				clickBlock.Y = std::min(clickBlock.Y, newMenuRect.Y);
				clickBlock.W = right - clickBlock.X;
				clickBlock.H = bottom - clickBlock.Y;
			}
			ctx.ClosePopupsOnOutsideClick({ toolbarPopup }, clickBlock);
		}
		else if (m_NewMenuOwner == 1)
		{
			// 父菜单被关掉(点外面/Esc/执行了菜单项)→ 子菜单不能再留着。
			m_NewMenuOwner = 0;
		}

		const Wui::WuiId treePopup = Wui::HashId("browser.tree.context");
		if (ctx.IsPopupOpen(treePopup) && !m_TreeMenuPath.empty())
		{
			// 根行可新建/刷新/打开,但重命名/删除内容根会让整棵树失效 → 这两项对根行禁用。
			const bool treeRoot = m_TreeMenuPath == m_Model.Root;
			// CPPSRC-1:项目源码根下树的"新建文件夹/重命名/删除"全部锁死(理由见源码锁定文案)。
			const bool treeSourcesScope = m_Model.Scope == BrowserRootScope::ProjectSources;
			// PLUG-T3:项目插件根同一套只读语义(共用同一条理由文案)。
			const bool treePluginsScope = m_Model.Scope == BrowserRootScope::ProjectPlugins;
			const bool treeReadOnlyScope = treeSourcesScope || treePluginsScope;
			const std::string treeLockedReason = Wui::Tr("panel.content_browser.src.locked_reason",
				"C++ sources are built by CMake — rename or delete them in Visual Studio or File Explorer.");
			struct TreeMenuItem
			{
				const char* Label;
				Wui::WuiId Id;
				bool Enabled;
				std::function<void()> Action;
			};
			const std::vector<TreeMenuItem> items = {
				{ "New Folder", Wui::HashId("browser.tree.menu.newfolder"), !treeReadOnlyScope,
					[this, &ctx]
					{
						const std::filesystem::path parent = m_TreeMenuPath;
						// 父行刚被右键过说明它已可见;展开它保证新建的子行能看到(重命名输入框画在那里)。
						m_Model.TreeOpen.insert(parent);
						const std::filesystem::path created = CreateFolderIn(ctx, parent);
						if (!created.empty())
							m_TreeRenameTarget = created;
					} },
				{ "Rename", Wui::HashId("browser.tree.menu.rename"), !treeRoot && !treeReadOnlyScope,
					[this, &ctx]
					{
						StartRename(ctx, m_TreeMenuPath);
						m_TreeRenameTarget = m_TreeMenuPath;
					} },
				{ "Delete", Wui::HashId("browser.tree.menu.delete"), !treeRoot && !treeReadOnlyScope,
					[this]
					{
						// 复用内容区删除流程:选中该行 → 现有删除确认弹窗 → DeleteSelection。
						m_Model.Selected.clear();
						m_Model.Selected.insert(m_TreeMenuPath);
						m_Model.LastSelected = m_TreeMenuPath;
						m_Model.ShowDeleteModal = true;
					} },
				{ "Open in Explorer", Wui::HashId("browser.tree.menu.openinexplorer"), true,
					[this] { OpenFolderInExplorer(m_TreeMenuPath); } },
				{ "Refresh", Wui::HashId("browser.tree.menu.refresh"), true,
					[this]
					{
						InvalidateContents();
						if (m_Model.Search[0])
							UpdateSearch();
					} },
			};
			// 与内容区两个菜单同一套组件:位置钉住 + 外部点击/Esc 关闭。
			Wui::WuiRect menuPanel;
			if (Wui::BeginContextMenu(ctx, treePopup, m_TreeMenuPos, 180.0f, items.size(), &menuPanel, theme))
			{
				for (size_t i = 0; i < items.size(); ++i)
				{
					const Wui::WuiRect item { menuPanel.X + 4, menuPanel.Y + 4 + i * 22, menuPanel.W - 8, 22 };
					if (Wui::ContextMenuItem(ctx, items[i].Id, item, items[i].Label, theme, items[i].Enabled))
					{
						items[i].Action();
						if (m_Ctx) m_Ctx->RecordOp("menu", "item", items[i].Label, "browser-tree");
						ctx.CloseAllPopups();
					}
					else if (!items[i].Enabled && treeReadOnlyScope)
					{
						// 与内容区右键菜单同一口径:禁用项的理由常驻在 a11y 节点上(悬停时再弹气泡)。
						if (ctx.IsHovered(item))
							ctx.SetTooltip(treeLockedReason);
						Wui::WuiAccessNode node;
						node.Id = items[i].Id;
						node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
						node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
						node.Kind = "menu-item";
						node.Label = items[i].Label;
						node.Tooltip = treeLockedReason;
						node.Rect = item;
						node.Enabled = false;
						node.Interactive = true;
						node.Visible = true;
						Wui::WuiAccessibility::Get().Register(node);
					}
				}
				if (ctx.IsKeyPressed(KeyCodes::Escape))
					ctx.ClosePopup(treePopup);
				Wui::EndContextMenu(ctx, treePopup, menuPanel, theme);
			}
		}
		else
		{
			// 菜单已关闭/无目标 → 清掉跨帧目标,避免下一帧按旧位置画出孤儿菜单。
			m_TreeMenuPath.clear();
			if (ctx.IsPopupOpen(treePopup))
				ctx.ClosePopup(treePopup);
		}

		// ---- 右侧内容区 ----
		const Wui::WuiRect content { rect.X + treeW + 6, y, rect.W - treeW - 6, rect.H - (y - rect.Y) };
		const bool searching = m_Model.Search[0] != 0;
		if (!searching)
			RefreshListing();
		const std::vector<std::filesystem::path>& paths = searching ? m_Model.SearchResults : m_Model.Listing;

		if (fileDrag && ctx.IsHovered(content))
		{
			if (m_Model.Scope != BrowserRootScope::ProjectPlugins)
			{
				ctx.DropTarget(content, "file:");
				m_Model.PendingDropDest = m_Model.Current;
				Wui::HighlightOutline(ctx, content, theme.Accent, 0.0f, 2.0f);
			}
		}

		// ---- P4-UX14:内容区统一切片 ----
		// 网格与列表共用同一份切片数据(图标 + 名称 + 类型/大小);排序与绘制都基于它。
		// 排序状态走 ctx.Persist(与控件状态同一条路):0=名称 1=类型 2=大小,-1=未排序。
		int& sortColumn = ctx.Persist<int>(Wui::HashId("browser.list.sort"), -1);
		bool& sortAscending = ctx.Persist<bool>(Wui::HashId("browser.list.sort.asc"), true);
		std::vector<BrowserSlice> slices;
		slices.reserve(paths.size());
		for (const std::filesystem::path& path : paths)
		{
			std::error_code dirError;
			BrowserSlice slice;
			slice.Path = path;
			slice.IsDir = std::filesystem::is_directory(path, dirError);
			// P4-UX14:后缀已经在切片底部单独一行显示,名字里就不再重复
			// (用户:"后缀名放底部这个设计很好,所以文件名部分就不需要再显示后缀名了")。
			slice.Name = path.extension().empty() ? path.filename().string() : path.stem().string();
			const EditorAssetType type = DescribeAssetType(path, slice.IsDir);
			slice.Type = type.Name;
			slice.Kind = type.Kind;
			slice.Extension = LowerExtension(path);
			// CPPSRC-1:项目源码根下的 `Generated/**` = 构建生成物(schema 注册 + Game.manifest 同步)。
			slice.GeneratedSource = m_Model.Scope == BrowserRootScope::ProjectSources
				&& !slice.IsDir && IsGeneratedSourcePath(path.lexically_relative(m_Model.Root));
			// PLUG-T3:项目插件根的清单文件 —— 类型列显示「插件清单」而不是裸 `.yaml`。
			slice.PluginManifest = !slice.IsDir && IsPluginManifestPath(path);
			// M4-TEX P4:源图行的两枚徽标输入(有资产 / 已烘焙)。判定要读源字节算 sha256,
			// 所以按 (源图 / .wtex / .wtexc) 的 mtime 缓存,只有真变了才重算。
			if (slice.Kind == EditorAssetKind::TextureSource && !slice.IsDir)
			{
				slice.TextureArtifact = ResolveTextureBadge(path, &slice.HasTextureAsset);
				slice.TextureArtifactKnown = true;
			}
			// P4-U10:切片上显示的类型文案走本地化;glTF/GLB 明确写成"导入源(导入用)",
			// 不再和原生 .wmodel 混在一起(用户 2026-09-21 报的歧义)。
			switch (type.Kind)
			{
				case EditorAssetKind::Folder: slice.TypeLabel = Wui::Tr("asset.file.folder", "Folder"); break;
				case EditorAssetKind::Scene: slice.TypeLabel = Wui::Tr("asset.file.scene", "Scene"); break;
				case EditorAssetKind::Material: slice.TypeLabel = Wui::Tr("asset.file.material", "Material"); break;
				case EditorAssetKind::Shader:
					// Slang-B1:唯一扩展名 `.slang` → 一个类型名。
					slice.TypeLabel = Wui::Tr("asset.file.shader", "Material Shader");
					break;
				case EditorAssetKind::Model: slice.TypeLabel = Wui::Tr("asset.file.model", "Model"); break;
				case EditorAssetKind::ModelSource:
					slice.TypeLabel = Wui::Tr("asset.file.model_source", "glTF source (import only)"); break;
				// M4-TEX P4:`.wtex` = 纹理资产(设置 + source:),图片 = 它的源(材质引用源图路径)。
				case EditorAssetKind::TextureAsset:
					slice.TypeLabel = Wui::Tr("asset.file.texture", "Texture");
					break;
				case EditorAssetKind::TextureSource:
					slice.TypeLabel = Wui::Tr("asset.file.texture_source", "Texture source");
					break;
				case EditorAssetKind::Script:
					// PECS-T11:Lua 类型列按**目录**给名(systems/ → Lua System,lib/ → Lua Library,
					// 其余 → Lua Script);名字与本地化 key 由 DescribeAssetType 按路径给出。
					slice.TypeLabel = Wui::Tr(type.LocalizationKey ? type.LocalizationKey : "asset.file.script",
						type.Name);
					break;
				// CPPSRC-1:项目层 C++ 源码(内容根里一般不出现,项目源码根下是主角)。
				case EditorAssetKind::CppHeader:
					slice.TypeLabel = Wui::Tr("asset.file.cpp_header", "C++ Header");
					break;
				case EditorAssetKind::CppSource:
					slice.TypeLabel = Wui::Tr("asset.file.cpp_source", "C++ Source");
					break;
				default: slice.TypeLabel = type.Name; break;
			}
			if (slice.PluginManifest)
				slice.TypeLabel = Wui::Tr("panel.content_browser.type.plugin_manifest", "Plugin Manifest");
			// 列表模式本来就要显示"大小"列:整表统计沿用旧行为;
			// 网格模式只对**可见**切片按需 stat(见 RenderGridSlices),大目录不做全量 stat。
			if (m_Model.ListMode)
			{
				slice.Size = slice.IsDir ? 0 : FileSize(path);
				slice.SizeKnown = true;
			}
			slices.push_back(std::move(slice));
		}

		// 本次只排"面板内这一份显示顺序":m_Model.Listing 与磁盘顺序都不动;
		// 搜索结果保持命中顺序,不参与排序(派工:只在 m_Model.Listing 上排序)。
		if (m_Model.ListMode && !searching && sortColumn >= 0 && slices.size() > 1)
		{
			std::stable_sort(slices.begin(), slices.end(),
				[&](const BrowserSlice& left, const BrowserSlice& right)
				{
					// 大小列:文件夹恒排前(升/降序都成立),只在同组内按字节数比较。
					if (sortColumn == 2 && left.IsDir != right.IsDir)
						return left.IsDir;
					int order = 0;
					if (sortColumn == 0)
					{
						// 名称 = 字典序(ASCII 大小写不敏感;全等时用原文做稳定补充)。
						order = LowerAscii(left.Name).compare(LowerAscii(right.Name));
						if (order == 0)
							order = left.Name.compare(right.Name);
					}
					else if (sortColumn == 1)
						order = left.Extension.compare(right.Extension);
					else
						order = left.Size < right.Size ? -1 : (left.Size > right.Size ? 1 : 0);
					// 同键时按名称、再按完整路径收尾:顺序稳定、可复现(不依赖扫描顺序)。
					if (order == 0)
						order = LowerAscii(left.Name).compare(LowerAscii(right.Name));
					if (order == 0)
						order = left.Path.generic_string().compare(right.Path.generic_string());
					return sortAscending ? order < 0 : order > 0;
				});
		}

		bool itemRightClicked = false;
		// D10:双击"进入文件夹/打开资产"必须**延迟到遍历结束**再执行 ——
		// `paths` 是 m_Model.SearchResults 的引用(Navigate→UpdateSearch 会清空重填),
		// 在循环里直接 OpenItem 会把正在遍历的容器清掉(迭代器失效 → 崩溃;
		// 用户实测"搜索后点击进入文件夹会报错")。
		std::optional<std::filesystem::path> pendingOpen;
		auto interact = [&](const std::filesystem::path& path, const Wui::WuiRect& itemRect, bool isDir)
		{
			const bool selected = m_Model.Selected.find(path) != m_Model.Selected.end();
			const bool hovered = ctx.IsHovered(itemRect);
			// P4-U10:`.gltf/.glb` 是**导入源**而不是可引用资产 —— 悬停直接说清"导入后场景引用
			// 产出的 .wmodel"(用户 2026-09-21:「gltf 和 wmodel 现在有歧义,尤其是网格选取时」)。
			if (hovered && !isDir)
			{
				const std::string extension = LowerExtension(path);
				if (extension == ".gltf" || extension == ".glb")
					Wui::Tooltip(ctx, itemRect, Wui::Tr("panel.content_browser.source_hint",
						"Import source (not referenceable): import it, then reference the produced .wmodel")
						+ "  →  " + path.stem().string() + ".wmodel");
			}
			if (isDir && fileDrag && hovered)
			{
				// PLUG-T3:项目插件根 = 只读浏览(拖入移动/导入锁死)。
				if (m_Model.Scope != BrowserRootScope::ProjectPlugins)
				{
					ctx.DropTarget(itemRect, "file:");
					m_Model.PendingDropDest = path;
					Wui::HighlightOutline(ctx, itemRect, theme.Accent, 2.0f, 2.0f);
				}
			}
			if (ctx.Input().MouseDown[0] && hovered
				&& m_Model.Scope != BrowserRootScope::ProjectPlugins)
			{
				const std::filesystem::path rel = path.lexically_relative(m_Model.Root);
				ctx.BeginDrag(Wui::HashId(("browser.drag." + rel.string()).c_str()), "file:" + rel.string());
				ctx.SetCursor(Wui::WuiCursor::Hand);
			}
			if (ctx.IsDoubleClicked(itemRect))
				pendingOpen = path;
			else if (ctx.IsClicked(itemRect))
			{
				if (ctx.Input().Ctrl)
				{
					if (selected) m_Model.Selected.erase(path);
					else m_Model.Selected.insert(path);
				}
				else if (ctx.Input().Shift && !m_Model.LastSelected.empty())
				{
					// 范围选择按**当前显示顺序**(列表排序后就是用户看到的顺序)取区间。
					auto start = std::find_if(slices.begin(), slices.end(),
						[&](const BrowserSlice& slice) { return slice.Path == m_Model.LastSelected; });
					auto end = std::find_if(slices.begin(), slices.end(),
						[&](const BrowserSlice& slice) { return slice.Path == path; });
					if (start != slices.end() && end != slices.end())
					{
						if (std::distance(start, end) < 0) std::swap(start, end);
						for (auto it = start; it <= end; ++it)
							m_Model.Selected.insert(it->Path);
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

		if (paths.empty())
		{
			// U2d:两种"空"分开表达 —— "目录本身为空"与"搜索无结果"不是一回事。
			const Wui::WuiRect emptyRect { content.X + 12.0f, content.Y + 12.0f,
				std::max(0.0f, content.W - 24.0f), std::max(0.0f, content.H - 24.0f) };
			if (searching)
			{
				const bool clearRequested = Wui::EmptyState(ctx, emptyRect, std::string(),
					Wui::Tr("panel.content_browser.search.empty_title", "No search results"),
					Wui::Tr("panel.content_browser.search.empty_hint",
						"Nothing matches in this folder or its subfolders. Try another keyword, or clear the search."),
					Wui::Tr("panel.content_browser.search.empty_action", "Clear Search"),
					Wui::HashId("browser.search.clear"), theme);
				if (clearRequested)
				{
					// 真的清掉搜索词并让下一帧重新扫描目录(搜索框缓冲与提交值一起清)。
					m_Model.Search[0] = 0;
					m_Model.SearchEdit.clear();
					m_Model.SearchResults.clear();
					m_Model.ListingDirty = true;
					m_Model.ListingStamp = {};
				}
			}
			else
			{
				const bool createRequested = Wui::EmptyState(ctx, emptyRect, std::string(),
					Wui::Tr("panel.content_browser.empty.title", "This folder is empty"),
					Wui::Tr("panel.content_browser.empty.hint",
						"Create a folder here, or drop a .gltf / .glb model into the window to import it."),
					Wui::Tr("panel.content_browser.empty.action", "New Folder"),
					Wui::HashId("browser.empty.newfolder"), theme);
				if (createRequested)
					CreateFolder(ctx);
			}
		}
		else if (m_Model.ListMode)
			// 列表:常驻表头(名称/类型/大小,点击列头在面板内排序)+ 图标/名称/类型/大小四列。
			RenderListSlices(ctx, content, theme, slices, sortColumn, sortAscending, interact, treeRenameDrawn);
		else
			// 网格:图标 → 名称 → 次级信息(扩展名 · 大小 / 文件夹),切片 ≥96×96。
			RenderGridSlices(ctx, content, theme, slices, interact, treeRenameDrawn);

		// D10:遍历结束后再执行"双击打开" —— 此时 Navigate→UpdateSearch 清空/重填
		// SearchResults 不会再破坏正在遍历的容器(见 pendingOpen 处的说明)。
		if (pendingOpen.has_value())
			OpenItem(*pendingOpen);

		if (ctx.AcceptDrop(&filePayload, "file:"))
		{
			// PLUG-T3:只读根不接受落下(拖入移动/导入锁死;落点也不会被 arm,这里再兜一层)。
			if (m_Model.Scope != BrowserRootScope::ProjectPlugins
				&& !m_Model.PendingDropDest.empty())
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
			// CPPSRC-1:条目多一个 Enabled(源码根下锁死重命名/删除/剪切/粘贴)与 Reason
			// (禁用理由 —— 悬停提示 + a11y Tooltip,灰项不能没有理由)。
			struct BrowserItem
			{
				std::string Label;
				std::function<void()> Action;
				bool Enabled = true;
				std::string Reason;
				// 无障碍 id 的稳定后缀(留空 = 沿用既有"按英文标签"的口径;本地化标签会变,
				// 所以新加的跨语言条目(如 open-vs)显式给 id)。
				std::string Id;
			};
			const bool single = m_Model.Selected.size() == 1;
			// 项目源码根 = 只读浏览(源码由 CMake 编译;schema 注册与 Game.manifest 依赖这些路径)。
			const bool sourcesScope = m_Model.Scope == BrowserRootScope::ProjectSources;
			// PLUG-T3:项目插件根同一套只读语义(共用同一条理由文案)。
			const bool pluginsScope = m_Model.Scope == BrowserRootScope::ProjectPlugins;
			const bool readOnlyScope = sourcesScope || pluginsScope;
			const std::string sourcesLockedReason = Wui::Tr("panel.content_browser.src.locked_reason",
				"C++ sources are built by CMake — rename or delete them in Visual Studio or File Explorer.");
			const std::string openInVsLabel = Wui::Tr("panel.content_browser.menu.open_vs",
				"Open in Visual Studio");
			const EditorAssetKind contextKind = std::filesystem::is_directory(m_Model.ContextMenuPath)
				? EditorAssetKind::Folder : DescribeAssetType(m_Model.ContextMenuPath, false).Kind;
			const bool contextIsCpp = contextKind == EditorAssetKind::CppHeader
				|| contextKind == EditorAssetKind::CppSource;
			// PLUG-T3:插件清单(`plugin.we.yaml`)在插件根下也走"Open in Visual Studio"。
			const bool contextIsPluginManifest = pluginsScope && contextKind != EditorAssetKind::Folder
				&& IsPluginManifestPath(m_Model.ContextMenuPath);
			const bool contextOpensInVs = contextIsCpp || contextIsPluginManifest;
			// P4-UX16:"新建 X"不再挂在**条目**右键菜单上 —— 它建在"当前文件夹",和条目无关
			// (资源管理器同款:新建属于空白处,条目菜单只做对该条目本身的操作)。
			// P4-U13:prefab 的菜单按"它是资产不是文件"来排 —— 打开编辑 / 实例化到当前场景
			// 排在最前(这两个才是日常动作),文件操作(剪切/复制…)跟在后面。
			const bool prefabFile =
				!std::filesystem::is_directory(m_Model.ContextMenuPath)
				&& LowerExtension(m_Model.ContextMenuPath) == ".wprefab";
			// M4-TEX P4:`.wtex` = 纹理资产(设置 / 重烘 / 回到默认);源图行只提供
			// "Create Texture Asset"(还没有资产时)—— 设置入口归资产行,源图不再单独承担。
			const std::string contextExtension = std::filesystem::is_directory(m_Model.ContextMenuPath)
				? std::string() : LowerExtension(m_Model.ContextMenuPath);
			const bool textureAssetFile = contextExtension == ".wtex";
			const bool textureSourceFile = TextureCompiler::IsTextureSourceExtension(contextExtension);
			const bool textureSourceHasAsset = textureSourceFile
				&& std::filesystem::exists(
					TextureAssetPathForSource(m_Model.ContextMenuPath.generic_string()));
			std::vector<BrowserItem> items;
			if (prefabFile)
			{
				// P4-U13c:Open = 打开资产窗口(与双击同一条);编辑是单独一条(文档会话)。
				items.push_back({ Wui::Tr("panel.content_browser.item.open_prefab", "Open Prefab Window"),
					[this] { OpenItem(m_Model.ContextMenuPath); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.edit_prefab", "Edit Prefab"),
					[this]
					{
						const std::filesystem::path contentRoot = m_Model.Root;
						std::error_code ec;
						const std::filesystem::path relative =
							std::filesystem::relative(m_Model.ContextMenuPath, contentRoot, ec);
						const std::string logical =
							ec ? m_Model.ContextMenuPath.generic_string() : relative.generic_string();
						m_Host.OpenPrefabEditor(logical);
					} });
				items.push_back({ Wui::Tr("panel.content_browser.item.instantiate", "Instantiate in Scene"),
					[this]
					{
						const std::filesystem::path contentRoot = m_Model.Root;
						std::error_code ec;
						const std::filesystem::path relative =
							std::filesystem::relative(m_Model.ContextMenuPath, contentRoot, ec);
						const std::string logical =
							ec ? m_Model.ContextMenuPath.generic_string() : relative.generic_string();
						std::string message;
						if (!m_Host.InstantiatePrefabAsset(logical, &message))
							WLD_CORE_WARN("[prefab] instantiate failed: {0}", message);
						else
							WLD_CORE_INFO("[prefab] {0}", message);
					} });
				items.push_back({ Wui::Tr("panel.content_browser.item.rename", "Rename"),
					[this, single] { if (single && m_Ctx) StartRename(*m_Ctx, m_Model.ContextMenuPath); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.reveal", "Show in Explorer"),
					[this] { OpenInExplorer(m_Model.ContextMenuPath); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.delete", "Delete"),
					[this] { m_Model.ShowDeleteModal = true; } });
			}
			else if (textureAssetFile)
			{
				items.push_back({ Wui::Tr("panel.content_browser.item.texture_settings", "Texture Settings…"),
					[this] { OpenTextureSettingsFor(m_Model.ContextMenuPath, false); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.texture_reimport", "Reimport"),
					[this] { ReimportTextureAsset(m_Model.ContextMenuPath); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.texture_reset", "Reset to Defaults"),
					[this] { OpenTextureSettingsFor(m_Model.ContextMenuPath, true); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.rename", "Rename"),
					[this, single] { if (single && m_Ctx) StartRename(*m_Ctx, m_Model.ContextMenuPath); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.reveal", "Show in Explorer"),
					[this] { OpenInExplorer(m_Model.ContextMenuPath); } });
				items.push_back({ Wui::Tr("panel.content_browser.item.delete", "Delete"),
					[this] { m_Model.ShowDeleteModal = true; } });
			}
			else
			{
				items = {
					// CPPSRC-1:项目源码根下"Open"就是"用 Visual Studio 打开"(双击同一条路径)。
					{ contextOpensInVs ? openInVsLabel : std::string("Open"),
						[this] { OpenItem(m_Model.ContextMenuPath); }, true, std::string(),
						contextOpensInVs ? std::string("open-vs") : std::string() },
					{ "Cut", [this] { Cut(); } },
					{ "Copy", [this] { Copy(); } },
					{ "Paste", [this] { PasteInto(std::filesystem::is_directory(m_Model.ContextMenuPath) ? m_Model.ContextMenuPath : m_Model.Current); } },
					{ "Rename", [this, single] { if (single && m_Ctx) StartRename(*m_Ctx, m_Model.ContextMenuPath); } },
					{ "Open in Explorer", [this] { OpenInExplorer(m_Model.ContextMenuPath); } },
					{ "Delete", [this] { m_Model.ShowDeleteModal = true; } },
				};
				if (readOnlyScope)
				{
					// 锁死:改名/删除/剪切/粘贴会破坏 CMake glob 与 schema/清单依赖 ——
					// 保留菜单项但禁用 + 理由(用户能看到"为什么不能点"),而不是静默消失。
					for (BrowserItem& entry : items)
					{
						const bool destructive = entry.Label == "Cut" || entry.Label == "Paste"
							|| entry.Label == "Rename" || entry.Label == "Delete";
						if (destructive)
						{
							entry.Enabled = false;
							entry.Reason = sourcesLockedReason;
						}
					}
				}
				// 还没有资产的源图:排在最前的一条 = 建资产(设置的家就是它,见 P0 §3)。
				if (textureSourceFile && !textureSourceHasAsset)
				{
					items.insert(items.begin(), BrowserItem {
						Wui::Tr("panel.content_browser.item.create_texture_asset", "Create Texture Asset"),
						[this] { CreateTextureAssetFor(m_Model.ContextMenuPath); } });
				}
			}
			// 右键菜单走组件(ContextMenu):位置钉住 + 外部点击/Esc 关闭统一处理。
			Wui::WuiRect menuPanel;
			if (Wui::BeginContextMenu(ctx, popup, m_Model.ContextMenuPos, 180.0f, items.size(), &menuPanel, theme))
			{
				for (size_t i = 0; i < items.size(); ++i)
				{
					const Wui::WuiRect item { menuPanel.X + 4, menuPanel.Y + 4 + i * 22, menuPanel.W - 8, 22 };
					const std::string idSuffix = items[i].Id.empty() ? items[i].Label : items[i].Id;
					const Wui::WuiId itemId = Wui::HashId(("browser.item." + idSuffix).c_str());
					if (Wui::ContextMenuItem(ctx, itemId, item, items[i].Label, theme, items[i].Enabled))
					{
						items[i].Action();
						if (m_Ctx) m_Ctx->RecordOp("menu", "item", items[i].Label, "browser");
						ctx.CloseAllPopups();
					}
					else if (!items[i].Enabled && !items[i].Reason.empty())
					{
						// 禁用项的理由:MenuItem 的 a11y 节点没有 tooltip 参数,这里按同一 id 重登记
						// (Register 是 upsert,以最后一次为准)⇒ **任何时候**读屏/脚本都读得到原因;
						// 鼠标真停在这一行上时再弹出气泡。
						if (ctx.IsHovered(item))
							ctx.SetTooltip(items[i].Reason);
						Wui::WuiAccessNode node;
						node.Id = itemId;
						node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
						node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
						node.Kind = "menu-item";
						node.Label = items[i].Label;
						node.Tooltip = items[i].Reason;
						node.Rect = item;
						node.Enabled = false;
						node.Interactive = true;
						node.Visible = true;
						Wui::WuiAccessibility::Get().Register(node);
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
			// P4-UX16:新建收成一行 "New ▶"(清单来自注册表)+ Paste + Refresh。
			const Wui::WuiRect menuPanel { m_Model.BlankMenuPos.x, m_Model.BlankMenuPos.y, 180, 3 * 24 + 8 };
			DrawPanelSurface(ctx, menuPanel, theme);
			// P4-U7:同上(空白右键菜单)。
			ctx.RegisterOverlayRect(menuPanel);
			const Wui::WuiRect newRow { menuPanel.X + 4, menuPanel.Y + 4, menuPanel.W - 8, 22 };
			const bool blankSourcesScope = m_Model.Scope == BrowserRootScope::ProjectSources;
			const bool blankPluginsScope = m_Model.Scope == BrowserRootScope::ProjectPlugins;
			if (blankSourcesScope)
			{
				// CPPSRC-1:源码根下空白右键只有"新建 C++ 组件…"(资产类型都不适用);
				// 走宿主同一个向导(File ▸ 新建 C++ 组件…)。
				const std::string newCppLabel = Wui::Tr("panel.content_browser.toolbar.new_cpp", "New C++ …");
				if (MenuItem(ctx, Wui::HashId("browser.blank.new.cpp"), newRow, newCppLabel, true, theme))
					m_Host.RequestNewCppScript();
			}
			else if (blankPluginsScope)
			{
				// PLUG-T3:项目插件根 = 只读浏览 —— 空白处"新建"同样锁死(理由与项目 C++ 根共用)。
				const std::string newLockedLabel = Wui::Tr("panel.content_browser.toolbar.new_locked", "New…");
				const Wui::WuiId newLockedId = Wui::HashId("browser.blank.new.locked");
				if (!MenuItem(ctx, newLockedId, newRow, newLockedLabel, false, theme))
					RegisterDisabledMenuItem(ctx, newLockedId, newRow, newLockedLabel,
						Wui::Tr("panel.content_browser.src.locked_reason",
							"C++ sources are built by CMake — rename or delete them in Visual Studio or File Explorer."));
			}
			else if (RenderNewAssetRow(ctx, Wui::HashId("browser.blank.new"), newRow, theme))
				m_NewMenuOwner = (m_NewMenuOwner == 2) ? 0 : 2;
			struct BlankItem { const char* Label; std::function<void()> Action; };
			const std::vector<BlankItem> items = {
				{ "Paste", [this] { PasteInto(m_Model.Current); } },
				{ "Refresh", [this] { InvalidateContents(); if (m_Model.Search[0]) UpdateSearch(); } },
			};
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Wui::WuiRect item { menuPanel.X + 4, menuPanel.Y + 4 + (i + 1) * 24, menuPanel.W - 8, 22 };
				const bool pasteLocked = (blankSourcesScope || blankPluginsScope)
					&& std::strcmp(items[i].Label, "Paste") == 0;
				if (MenuItem(ctx, Wui::HashId(("browser.blank." + std::string(items[i].Label)).c_str()), item,
						items[i].Label, !pasteLocked, theme))
				{
					items[i].Action();
					if (m_Ctx) m_Ctx->RecordOp("menu", "item", items[i].Label, "browser-blank");
					ctx.CloseAllPopups();
				}
				// 禁用项的理由(与内容区右键菜单同一口径)。
				else if (pasteLocked && ctx.IsHovered(item))
					ctx.SetTooltip(Wui::Tr("panel.content_browser.src.locked_reason",
						"C++ sources are built by CMake — rename or delete them in Visual Studio or File Explorer."));
			}
			Wui::WuiRect newMenuRect;
			if (m_NewMenuOwner == 2 && !blankSourcesScope && !blankPluginsScope)
				newMenuRect = RenderNewAssetItems(ctx, "browser.blank.new.", menuPanel, rect, theme);
			// 父菜单 + 展开的"新建"清单算同一块点击区(否则点子菜单会把父菜单一起关掉)。
			Wui::WuiRect clickBlock = menuPanel;
			if (newMenuRect.W > 0.0f)
			{
				const float right = std::max(menuPanel.X + menuPanel.W, newMenuRect.X + newMenuRect.W);
				const float bottom = std::max(menuPanel.Y + menuPanel.H, newMenuRect.Y + newMenuRect.H);
				clickBlock.X = std::min(clickBlock.X, newMenuRect.X);
				clickBlock.Y = std::min(clickBlock.Y, newMenuRect.Y);
				clickBlock.W = right - clickBlock.X;
				clickBlock.H = bottom - clickBlock.Y;
			}
			ctx.ClosePopupsOnOutsideClick({ blankPopup }, clickBlock);
			if (ctx.IsKeyPressed(KeyCodes::Escape))
			{
				if (m_NewMenuOwner == 2)
					m_NewMenuOwner = 0;
				else
					ctx.ClosePopup(blankPopup);
			}
			ctx.PopOverlay();
		}
		else if (m_NewMenuOwner == 2)
		{
			m_NewMenuOwner = 0;
		}

		// ---- 删除确认 ----
		const Wui::WuiId deleteModal = Wui::HashId("browser.delete");
		if (m_Model.ShowDeleteModal && (ctx.Modal() == 0 || ctx.Modal() == deleteModal))
			ctx.SetModal(deleteModal);
		else if (!m_Model.ShowDeleteModal && ctx.Modal() == deleteModal)
			ctx.ClearModal();
		Wui::WuiRect panel;
		bool escapePressed = false;
		// D10-11:与导入位置模态共用 WuiModal 组件(居中/遮罩/标题栏/Esc/按钮条);
		// 文案与 id 逐字不变(Yes → DeleteSelection,No/Esc → 关)。
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = deleteModal;
		frameDesc.Title = "Delete Confirmation";
		frameDesc.Size = { 380.0f, 160.0f };
		if (Wui::BeginModalFrame(ctx, frameDesc, &panel, &escapePressed, theme))
		{
			Label(ctx, { panel.X + 16, panel.Y + 48 }, "Delete " + std::to_string(m_Model.Selected.size()) + " item(s)? This cannot be undone.", theme.Text, 14.0f);
			const Wui::ModalResult result = Wui::ModalFooter(ctx, panel, "Yes", "No",
				Wui::HashId("browser.delete.yes"), Wui::HashId("browser.delete.no"), true, theme);
			if (result == Wui::ModalResult::Confirm)
			{
				DeleteSelection();
				m_Model.ShowDeleteModal = false;
				ctx.ClearModal();
			}
			else if (result == Wui::ModalResult::Cancel || escapePressed)
			{
				m_Model.ShowDeleteModal = false;
				ctx.ClearModal();
			}
			Wui::EndModalFrame(ctx);
		}

		// ---- U25-M2:E 新建材质向导(面板级模态;外壳按 m_PanelModalOwner 封锁其余面板输入)----
		DrawNewMaterialModal(ctx);
		// M4-S2/Slang-B1:Material Shader 新建向导(同一套面板级模态语义;两个模态不会同时打开)。
		DrawNewShaderModal(ctx);

		// ---- 面板级快捷键(放在**最后**判定)----
		// 关键:重命名输入框是在本函数后半段才绘制的,`SetTextInputActive` 也是那时才登记
		// 文本焦点;若在前面判定,本帧的 WuiTextFocus 还是空的 → Ctrl+A 会去全选文件夹
		// (用户实测两次)。放到函数末尾,读到的就是本帧已经登记好的焦点状态。
		const std::vector<std::filesystem::path>& shortcutPaths =
			searching ? m_Model.SearchResults : m_Model.Listing;
		const bool textFocusActive = Wui::WuiTextFocus::Get().Active();
		if (!textFocusActive && ctx.Input().Ctrl && ctx.IsKeyPressed(KeyCodes::A) && ctx.IsHovered(content))
			SelectAll(shortcutPaths);
		// CPPSRC-1:项目源码根 = 只读浏览 —— Delete/F2 这两条会把源码删掉/改名的键盘路整条不触发
		// (菜单项已灰显 + 理由;键盘必须同一条口径,不能"菜单点不动、按 Delete 就删了")。
		// PLUG-T3:项目插件根同一套(只读浏览)。
		const bool readOnlyScope = m_Model.Scope != BrowserRootScope::Content;
		if (!textFocusActive && !readOnlyScope
			&& ctx.IsKeyPressed(KeyCodes::Delete) && !m_Model.Selected.empty() && ctx.IsHovered(content))
			m_Model.ShowDeleteModal = true;
		if (!textFocusActive && !readOnlyScope
			&& ctx.IsKeyPressed(KeyCodes::F2) && m_Model.Selected.size() == 1 && ctx.IsHovered(content))
			StartRename(ctx, *m_Model.Selected.begin());

		// ---- P4-UX16:由 OnShortcut(第 2 层路由)排队的"新建"动作 ----
		// 快捷键事件在渲染之外到达,那里没有 ctx;这里执行真正的动作,与菜单走同一条
		// CreateAssetFromRegistry 路径(所以注册表里新增的类型同样自动获得快捷键语义)。
		if (m_PendingNewShortcut != 0)
		{
			const int request = m_PendingNewShortcut;
			m_PendingNewShortcut = 0;
			if (m_Model.Scope == BrowserRootScope::ProjectSources)
			{
				// CPPSRC-1:源码根下 Ctrl+N = 新建 C++ 组件(资产类型都不适用);
				// Ctrl+Shift+N(新建文件夹)在只读浏览下锁死,只给理由,不做事。
				if (request == 2)
					NotifyAssetFailure(Wui::Tr("panel.content_browser.src.locked_reason",
						"C++ sources are built by CMake — rename or delete them in Visual Studio or File Explorer."));
				else if (!m_Host.RequestNewCppScript())
					NotifyAssetFailure(Wui::Tr("panel.content_browser.new_cpp.unavailable",
						"Open a project first — the New C++ … wizard writes into <project>/src/."));
			}
			else if (m_Model.Scope == BrowserRootScope::ProjectPlugins)
			{
				// PLUG-T3:项目插件根 = 只读浏览 —— Ctrl+N / Ctrl+Shift+N 都只给理由,不做事。
				NotifyAssetFailure(Wui::Tr("panel.content_browser.src.locked_reason",
					"C++ sources are built by CMake — rename or delete them in Visual Studio or File Explorer."));
			}
			else if (request == 2)
			{
				std::string error;
				if (!CreateAssetFromRegistry("folder", &error))
					NotifyAssetFailure(error);
			}
			else
			{
				// Ctrl+N:打开 `…` 菜单并直接展开"新建"清单(键盘可达,不必先点 ⋯)。
				m_ToolbarMenuOpen = true;
				m_NewMenuOwner = 1;
			}
		}
	}

}
