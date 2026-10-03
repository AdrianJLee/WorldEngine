#include "MaterialEditorPanel_Internal.h"

namespace World
{

using namespace MaterialEditorPanelDetail;


	// ---- U25-M2:C 引用者(谁在用这个材质)----
	//
	// 口径(方案 §C):扫内容根里的 .wd / .wprefab / .wmodel,**直接文本匹配**本材质的逻辑路径
	// (归一化分隔符 + 大小写不敏感),不解析字段语义;带 2s TTL,不每帧读盘。
	// 零引用 = 明确写"无引用"(不是空白);扫描失败 = 把可读原因显示出来。
void MaterialEditorPanel::RefreshReferences(double now, bool force){
		(void)force;
		m_RefsTime = now;
		m_RefsPath = m_Path;
		m_Refs.clear();
		m_RefsError.clear();
		if (m_Path.empty())
			return;   // 未落盘的材质没有资产路径,别人引用不到它
		const std::filesystem::path root = ContentRootPath();
		std::error_code rootError;
		if (!std::filesystem::is_directory(root, rootError))
		{
			m_RefsError = Wui::Tr("panel.material.refs.error.root", "content root not found: ")
				+ root.generic_string();
			return;
		}
		const std::string needle = ToLowerAscii(m_Path);
		std::error_code walkError;
		for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root,
			std::filesystem::directory_options::skip_permission_denied, walkError))
		{
			std::error_code typeError;
			if (!entry.is_regular_file(typeError))
				continue;
			const std::string extension = LowerExtension(entry.path().string());
			const char* type = nullptr;
			if (extension == ".wd")
				type = "scene";
			else if (extension == ".wprefab")
				type = "prefab";
			else if (extension == ".wmodel")
				type = "model";
			if (!type)
				continue;
			std::error_code sizeError;
			const uintmax_t size = std::filesystem::file_size(entry.path(), sizeError);
			if (sizeError || size > 16u * 1024u * 1024u)
				continue;   // 超大文件不当文本读(引用一定写在文本头/场景行里)
			std::ifstream file(entry.path(), std::ios::binary);
			if (!file)
				continue;
			std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			if (text.empty())
				continue;
			std::replace(text.begin(), text.end(), '\\', '/');
			if (ToLowerAscii(text).find(needle) == std::string::npos)
				continue;
			std::error_code relativeError;
			const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, relativeError);
			if (relativeError)
				continue;
			m_Refs.push_back({ relative.generic_string(), type });
		}
		if (walkError)
			m_RefsError = Wui::Tr("panel.material.refs.error.scan", "reference scan failed: ")
				+ walkError.message();
		std::sort(m_Refs.begin(), m_Refs.end(),
			[](const RefEntry& left, const RefEntry& right) { return left.Path < right.Path; });
	}


float MaterialEditorPanel::DrawReferences(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host){
		if (rect.H <= 0.0f || rect.W <= 0.0f)
			return 0.0f;
		const Wui::WuiTheme& theme = host.Theme();
		const size_t shown = std::min(m_Refs.size(), kRefsMaxShown);
		char counted[192] = {};
		std::snprintf(counted, sizeof(counted),
			Wui::Tr("panel.material.refs.count", "Referenced by %zu asset(s)").c_str(), m_Refs.size());
		std::string label;
		std::string tooltip;
		if (!m_RefsError.empty())
		{
			label = Wui::Tr("panel.material.refs.error.label", "Reference scan failed");
			tooltip = m_RefsError;
		}
		else if (m_Refs.empty())
		{
			// 零引用必须写出来(方案 §C:不是空白)。
			label = Wui::Tr("panel.material.refs.none", "No references");
			tooltip = Wui::Tr("panel.material.refs.none.tooltip",
				"No scene / prefab / model under the content root mentions this material path yet.");
		}
		else
		{
			label = counted;
			tooltip = Wui::Tr("panel.material.refs.tooltip",
				"Click to list the scenes / prefabs / models that mention this material, then click an entry "
				"to select it in the Content Browser. The scan is direct text matching, refreshed every 2s.");
		}
		const std::string display = std::string(m_RefsOpen ? "v  " : "▶  ") + label;
		const Wui::WuiRect rowRect { rect.X, rect.Y, rect.W, kRefsRowHeight };
		const bool hovered = ctx.IsHovered(rowRect);
		Wui::PanelBackground(ctx, { rect.X, rect.Y, rect.W, 1.0f }, theme.BorderStrong, 0.0f);
		if (hovered)
			Wui::PanelBackground(ctx,
				{ rect.X + 2.0f, rect.Y + 2.0f, std::max(20.0f, rect.W - 4.0f), kRefsRowHeight - 2.0f },
				theme.HoverBg, theme.Radius);
		Wui::Label(ctx, { rect.X + 6.0f, rect.Y + (kRefsRowHeight - 12.0f) * 0.5f },
			EllipsizeToWidth(ctx, display, std::max(20.0f, rect.W - 12.0f), 12.0f),
			m_RefsError.empty() ? theme.Text : theme.Danger, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.refs");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "button";
			node.Label = label;
			// value = 纯计数(脚本按它断言"引用者 +1");类型分解放在 tooltip 里。
			node.Value = std::to_string(m_Refs.size());
			node.Tooltip = tooltip;
			node.Rect = rowRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		Wui::DrawFocusRing(ctx, rowRect, Wui::HashId("material.refs"), theme);
		ctx.RegisterFocusable(Wui::HashId("material.refs"), rowRect);
		if (hovered)
		{
			ctx.SetCursor(Wui::WuiCursor::Hand);
			ctx.SetTooltip(tooltip);
		}
		if (ctx.IsClicked(rowRect))
		{
			m_RefsOpen = !m_RefsOpen;
			ctx.RecordOp("material", m_RefsOpen ? "refs-open" : "refs-close", m_Path,
				std::to_string(m_Refs.size()));
		}
		if (!m_RefsOpen)
			return rect.H;
		// 展开:逐条列出引用者(逻辑路径 + 类型);点一条 = 在内容浏览器里选中并定位它。
		for (size_t index = 0; index < shown; ++index)
		{
			const RefEntry& entry = m_Refs[index];
			const Wui::WuiRect itemRect { rect.X + 6.0f,
				rect.Y + kRefsRowHeight + static_cast<float>(index) * kRefsItemHeight,
				std::max(40.0f, rect.W - 12.0f), kRefsItemHeight - 1.0f };
			const bool itemHovered = ctx.IsHovered(itemRect);
			if (itemHovered)
				Wui::PanelBackground(ctx, itemRect, theme.HoverBg, 2.0f);
			const std::string typeLabel = Wui::Tr(("panel.material.refs.type." + entry.Type).c_str(),
				entry.Type == "prefab" ? "Prefab" : (entry.Type == "model" ? "Model" : "Scene"));
			const float typeWidth = std::min(64.0f, ctx.MeasureTextWidth(typeLabel, 11.0f) + 8.0f);
			Wui::Label(ctx, { itemRect.X + 4.0f, itemRect.Y + 3.0f },
				EllipsizeToWidth(ctx, entry.Path, itemRect.W - typeWidth - 12.0f, 12.0f), theme.Text, 12.0f);
			Wui::Label(ctx, { itemRect.X + itemRect.W - typeWidth, itemRect.Y + 4.0f }, typeLabel,
				theme.TextMuted, 11.0f);
			const std::string itemId = "material.refs.item." + entry.Path;
			const std::string itemTooltip = Wui::Tr("panel.material.refs.item.tooltip",
				"Select this asset in the Content Browser (and navigate to its folder).");
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId(itemId.c_str());
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "button";
			node.Label = entry.Path;
			node.Value = typeLabel;
			node.Tooltip = itemTooltip;
			node.Rect = itemRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
			ctx.RegisterFocusable(node.Id, itemRect);
			if (itemHovered)
			{
				ctx.SetCursor(Wui::WuiCursor::Hand);
				ctx.SetTooltip(itemTooltip);
			}
			if (ctx.IsClicked(itemRect))
			{
				if (host.SelectContentAsset(entry.Path, "material-ref"))
				{
					m_Status = Wui::Tr("panel.material.status.ref_located", "Selected in the Content Browser: ")
						+ entry.Path;
					m_StatusIsError = false;
				}
				else
				{
					m_Status = Wui::Tr("panel.material.status.ref_locate_failed",
						"Could not select that asset in the Content Browser: ") + entry.Path;
					m_StatusIsError = true;
				}
			}
		}
		if (m_Refs.size() > shown)
		{
			const std::string more = "+" + std::to_string(m_Refs.size() - shown) + " …";
			Wui::Label(ctx, { rect.X + 10.0f,
				rect.Y + kRefsRowHeight + static_cast<float>(shown) * kRefsItemHeight }, more,
				theme.TextMuted, 11.0f);
		}
		return rect.H;
	}


	// ---- U25-M2:A 把材质接回场景(Assign to Selection / 撤销本次赋值)----
void MaterialEditorPanel::AssignToSelection(PanelHost& host){
		if (!m_Material)
			return;
		if (m_Path.empty())
		{
			m_Status = Wui::Tr("panel.material.status.assign_nosave",
				"Assign failed: this material has never been saved, so it has no asset path to reference");
			m_StatusIsError = true;
			return;
		}
		Entity target;
		std::string previous;
		std::string message;
		if (!host.AssignMaterialToSelection(m_Path, &target, &previous, &message))
		{
			m_Status = Wui::Tr("panel.material.status.assign_failed", "Assign failed: ")
				+ (message.empty()
					? Wui::Tr("panel.material.status.no_selection", "no selection") : message);
			m_StatusIsError = true;
			return;
		}
		m_AssignUndoValid = target.IsValid();
		m_AssignUndoEntity = target;
		m_AssignUndoPath = previous;
		m_AssignUndoTarget = message;
		m_Status = Wui::Tr("panel.material.status.assigned", "Assign to Selection: ") + message;
		m_StatusIsError = false;
	}


void MaterialEditorPanel::UndoAssign(PanelHost& host){
		if (!m_AssignUndoValid)
		{
			m_Status = Wui::Tr("panel.material.status.undo_none",
				"Undo Assign: nothing to undo in this window");
			m_StatusIsError = true;
			return;
		}
		std::string message;
		if (!host.SetEntityMaterialPath(m_AssignUndoEntity, m_AssignUndoPath, &message))
		{
			m_Status = Wui::Tr("panel.material.status.undo_failed", "Undo Assign failed: ")
				+ (message.empty()
					? Wui::Tr("panel.material.status.entity_gone", "the entity is gone") : message);
			m_StatusIsError = true;
			// 实体已失效(被删除/换场景):不再提供撤销,避免反复失败。
			m_AssignUndoValid = false;
			m_AssignUndoEntity = Entity {};
			return;
		}
		m_Status = Wui::Tr("panel.material.status.undone", "Undo Assign: ") + message;
		m_StatusIsError = false;
		m_AssignUndoValid = false;
		m_AssignUndoEntity = Entity {};
		m_AssignUndoPath.clear();
		m_AssignUndoTarget.clear();
	}


	// ---- U25-M2:D Save As…(变体的入口)----
std::string MaterialEditorPanel::SaveAsTarget() const{
		const std::string name = MaterialBaseName(m_SaveAsName);
		std::string folder;
		if (m_SaveAsFolderIndex >= 0 && m_SaveAsFolderIndex < static_cast<int>(m_SaveAsFolders.size()))
			folder = m_SaveAsFolders[static_cast<size_t>(m_SaveAsFolderIndex)];
		std::string target = folder.empty() ? std::string() : (folder + "/");
		target += name.empty() ? std::string("(name)") : name;
		target += ".wmat";
		return target;
	}


std::string MaterialEditorPanel::SaveAsNameError() const{
		const std::string name = MaterialBaseName(m_SaveAsName);
		if (name.empty())
			return Wui::Tr("panel.material.saveas.name.empty", "Name cannot be empty");
		if (name == "." || name == "..")
			return Wui::Tr("panel.material.saveas.name.dot", "Name cannot be '.' or '..'");
		for (const char character : name)
			if (character == '\\' || character == '/' || character == ':' || character == '*'
				|| character == '?' || character == '"' || character == '<' || character == '>'
				|| character == '|')
				return Wui::Tr("panel.material.saveas.name.illegal",
					"Name cannot contain \\ / : * ? \" < > |");
		if (name.back() == '.' || name.back() == ' ')
			return Wui::Tr("panel.material.saveas.name.trailing", "Name cannot end with a dot or a space");
		// 方案 §D:不覆盖源文件 —— 目标就是源文件时禁用并说明。
		if (!m_Path.empty()
			&& MaterialLibrary::NormalizePath(SaveAsTarget()) == MaterialLibrary::NormalizePath(m_Path))
			return Wui::Tr("panel.material.saveas.name.source",
				"That is the source file — write the variant under another name (the source is never overwritten)");
		return {};
	}


void MaterialEditorPanel::OpenSaveAsModal(Wui::WuiContext& ctx, PanelHost& host){
		if (!m_Material)
			return;
		m_SaveAsOpen = true;
		m_OpenConfirmOpen = false;
		m_SaveAsOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		m_SaveAsFailure.clear();
		m_SaveAsFailureFor.clear();
		// 默认名 = <原名>_variant;默认目录 = 当前材质所在目录(未落盘时 materials/)。
		const std::filesystem::path source(m_Path);
		std::string stem = source.stem().string();
		if (stem.empty())
			stem = "material";
		m_SaveAsName = stem + "_variant";
		std::string folder = source.parent_path().generic_string();
		if (folder.empty())
			folder = "materials";
		m_SaveAsFolders = Editor::AssetCatalog::Dirs();
		auto found = std::find(m_SaveAsFolders.begin(), m_SaveAsFolders.end(), folder);
		if (found == m_SaveAsFolders.end())
		{
			m_SaveAsFolders.push_back(folder);
			std::sort(m_SaveAsFolders.begin(), m_SaveAsFolders.end());
			found = std::find(m_SaveAsFolders.begin(), m_SaveAsFolders.end(), folder);
		}
		m_SaveAsFolderIndex = found == m_SaveAsFolders.end()
			? 0 : static_cast<int>(found - m_SaveAsFolders.begin());
		ctx.SetModal(Wui::HashId("material.saveas.modal"));
		ctx.SetFocus(Wui::HashId("material.saveas.name"));
		ctx.RecordOp("material", "saveas-ask", m_Path, m_SaveAsName);
	}


void MaterialEditorPanel::CloseSaveAsModal(Wui::WuiContext& ctx, PanelHost& host){
		(void)host;
		m_SaveAsOpen = false;
		m_SaveAsFailure.clear();
		m_SaveAsFailureFor.clear();
		m_SaveAsFolders.clear();
		if (ctx.Modal() == Wui::HashId("material.saveas.modal"))
			ctx.ClearModal();
		ctx.ClosePopup(Wui::HashId("material.saveas.folder"));
	}


void MaterialEditorPanel::DrawSaveAsModal(Wui::WuiContext& ctx, PanelHost& host){
		if (!m_SaveAsOpen || !m_Material)
			return;
		const Wui::WuiId modalId = Wui::HashId("material.saveas.modal");
		const Wui::WuiTheme& theme = host.Theme();
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("panel.material.saveas.title", "Save Material As…");
		frameDesc.Size = { 560.0f, 350.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
		{
			// 模态被别处清掉:收回状态,不留悬空(与 Create Prefab 同一条规则)。
			CloseSaveAsModal(ctx, host);
			return;
		}
		const Wui::WuiId nameId = Wui::HashId("material.saveas.name");
		const Wui::WuiId folderId = Wui::HashId("material.saveas.folder");
		const bool folderPopupWasOpen = ctx.IsPopupOpen(folderId);
		const bool justOpened = ctx.Frame() == m_SaveAsOpenedFrame;
		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 110.0f;
		const float fieldW = frame.W - 126.0f - 72.0f;

		// ---- 名称(后缀 .wmat 自动补,常显在右侧)----
		Wui::Label(ctx, { labelX, frame.Y + 49.0f },
			Wui::Tr("panel.material.saveas.name", "Name"), theme.TextMuted, 13.0f);
		const Wui::WuiRect nameRect { fieldX, frame.Y + 44.0f, fieldW, 24.0f };
		NoteFocusOwningRect(nameRect);   // MAT-UI3b:模态里的名称输入自己接手焦点
		const bool nameFocused = ctx.Focus() == nameId;
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = Wui::Tr("panel.material.saveas.name", "Name");
		nameA11y.Placeholder = Wui::Tr("panel.material.saveas.name.placeholder", "Variant name");
		Wui::TextField(ctx, nameId, nameRect, m_SaveAsName, theme, nullptr, &nameA11y);
		const bool nameSubmitted = nameFocused && ctx.IsKeyPressed(KeyCodes::Enter);
		Wui::Label(ctx, { nameRect.X + nameRect.W + 8.0f, frame.Y + 50.0f }, ".wmat", theme.TextMuted, 13.0f);
		const std::string nameError = SaveAsNameError();
		if (!nameError.empty())
			Wui::Label(ctx, { fieldX, frame.Y + 71.0f }, nameError, theme.Danger, 12.0f);

		// ---- 目录(内容根下的目录,可搜索;缺目录会自动新建)----
		const std::string folderLabel = Wui::Tr("panel.material.saveas.folder", "Folder");
		Wui::Label(ctx, { labelX, frame.Y + 99.0f }, folderLabel, theme.TextMuted, 13.0f);
		const Wui::WuiRect folderRect { fieldX, frame.Y + 94.0f, fieldW, 24.0f };
		Wui::SearchableCombo(ctx, folderId, folderRect, folderLabel, m_SaveAsFolders,
			m_SaveAsFolderIndex, theme);
		const std::string folderText = (m_SaveAsFolderIndex >= 0
			&& m_SaveAsFolderIndex < static_cast<int>(m_SaveAsFolders.size()))
			? m_SaveAsFolders[static_cast<size_t>(m_SaveAsFolderIndex)] : std::string();
		{
			Wui::WuiAccessNode node;
			node.Id = folderId;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "search-combo";
			node.Label = folderLabel;
			node.Value = folderText;
			node.Tooltip = Wui::Tr("panel.material.saveas.folder.tooltip",
				"Folder under the content root (searchable); a missing folder is created on confirm.");
			node.Rect = folderRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		const std::filesystem::path folderAbsolute = ContentRootPath() / std::filesystem::path(folderText);
		if (!folderText.empty() && !std::filesystem::is_directory(folderAbsolute))
			Wui::Label(ctx, { fieldX, frame.Y + 121.0f },
				Wui::Tr("panel.material.saveas.folder.note",
					"Folder does not exist yet — it will be created"), theme.Warning, 12.0f);

		// ---- 实时落点回显 ----
		const std::string target = SaveAsTarget();
		if (!m_SaveAsFailure.empty() && m_SaveAsFailureFor != target)
		{
			m_SaveAsFailure.clear();
			m_SaveAsFailureFor.clear();
		}
		const std::string previewLabel = Wui::Tr("panel.material.saveas.preview.label", "Will create");
		Wui::Label(ctx, { labelX, frame.Y + 152.0f }, previewLabel, theme.TextMuted, 12.0f);
		Wui::Label(ctx, { fieldX, frame.Y + 150.0f }, target, theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.saveas.preview");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = previewLabel;
			node.Value = target;
			node.Tooltip = Wui::Tr("panel.material.saveas.preview.tooltip",
				"Logical path of the file that will be written (folder + name + .wmat).");
			node.Rect = { fieldX, frame.Y + 146.0f, fieldW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// ---- 目标已存在 = 红字 + 主按钮变 Overwrite(源文件永远不覆盖)----
		std::error_code existsError;
		const bool targetExists = nameError.empty() && std::filesystem::exists(
			ContentRootPath() / std::filesystem::path(target), existsError);
		std::string warningText;
		if (!m_SaveAsFailure.empty())
			warningText = m_SaveAsFailure;
		else if (targetExists)
			warningText = Wui::Tr("panel.material.saveas.exists", "Already exists — overwriting: ") + target;
		if (!warningText.empty())
			Wui::Label(ctx, { fieldX, frame.Y + 176.0f }, warningText, theme.Danger, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.saveas.warning");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.saveas.warning.label", "Warning");
			node.Value = warningText;
			node.Tooltip = Wui::Tr("panel.material.saveas.warning.tooltip",
				"Red line = the target exists and would be overwritten; empty = no conflict.");
			node.Rect = { fieldX, frame.Y + 172.0f, fieldW, 18.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		// ---- M3:变体语义说明(Parent = 当前材质 + 只写覆盖字段)----
		// 这里必须**写清**产物是什么:`Parent:` 指向当前材质,文件里只出现"当前材质自己
		// 覆盖过的字段",不是整份拷贝(源文件与源材质的继承链都不动)。
		const std::string variantNote = m_Path.empty()
			? Wui::Tr("panel.material.saveas.variant.unsaved",
				"This material has never been saved, so the variant starts as a full standalone copy "
				"(no Parent line).")
			: Wui::Tr("panel.material.saveas.variant.note",
				"Variant of") + " " + m_Path
				+ Wui::Tr("panel.material.saveas.variant.note2",
					": the file stores Parent: <this material> plus only the fields it overrides.");
		Wui::Label(ctx, { fieldX, frame.Y + 200.0f },
			EllipsizeToWidth(ctx, variantNote, fieldW, 12.0f), theme.TextMuted, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.saveas.parent");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.material.saveas.variant.label", "Parent of the variant");
			node.Value = m_Path;
			node.Tooltip = variantNote;
			node.Rect = { fieldX, frame.Y + 196.0f, fieldW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// ---- 底部按钮条:主按钮(Create Variant / Overwrite)+ Cancel ----
		const bool canCreate = nameError.empty();
		const float footerY = frame.Y + frame.H - Wui::ModalFooterPadding - Wui::ModalFooterHeight;
		const Wui::WuiRect okRect { frame.X + frame.W - 16.0f - 150.0f, footerY, 150.0f,
			Wui::ModalFooterHeight };
		const Wui::WuiRect cancelRect { okRect.X - 8.0f - 96.0f, footerY, 96.0f, Wui::ModalFooterHeight };
		const std::string okLabel = targetExists
			? Wui::Tr("panel.material.saveas.overwrite", "Overwrite")
			: Wui::Tr("panel.material.saveas.create", "Create Variant");
		const std::string okTooltip = !canCreate
			? (nameError + " — " + Wui::Tr("panel.material.saveas.ok.disabled",
				"fix the name to enable this button"))
			: (targetExists
				? Wui::Tr("panel.material.saveas.overwrite.tooltip",
					"The file exists: overwrite it with the current material as a variant")
				: Wui::Tr("panel.material.saveas.create.tooltip",
					"Write the variant and keep editing it in this window (the source .wmat stays untouched)"));
		const bool okClicked = ActionButton(ctx, Wui::HashId("material.saveas.ok"), okRect, okLabel,
			okTooltip, canCreate, true, theme);
		const bool cancelClicked = ActionButton(ctx, Wui::HashId("material.saveas.cancel"), cancelRect,
			Wui::Tr("panel.material.saveas.cancel", "Cancel"),
			Wui::Tr("panel.material.saveas.cancel.tooltip", "Close without writing anything (Esc)"),
			true, false, theme);

		bool closeRequested = false;
		if ((okClicked || (nameSubmitted && !justOpened)) && canCreate)
		{
			const std::string base = MaterialBaseName(m_SaveAsName);
			// M3:变体 = `Parent: <当前材质>` + 只写覆盖字段(不是整份拷贝;源文件与源材质的
			// 覆盖集都不动)。覆盖集照抄源材质 —— 源怎么写,变体就怎么写;源没写的字段
			// 继续沿着 Parent 链继承。v1 全字段老文件因此写出"全字段 + Parent"。
			std::string error;
			std::string savedPath;
			if (m_Path.empty())
			{
				// 未落盘材质没有文件可当父级:退回整份拷贝(旧口径),状态里会写清没有 Parent。
				Ref<Material> variant = MaterialLibrary::Get().CreateDefault(base);
				MaterialDesc desc = m_Material->GetDesc();
				desc.Name = base;
				variant->SetDesc(desc);
				if (MaterialLibrary::Get().Save(variant, target, &error))
					savedPath = variant->GetPath();
			}
			else
			{
				MaterialDocument document;
				document.ParentPath = MaterialLibrary::NormalizePath(m_Path);
				document.Values = m_Material->GetDesc();
				document.Values.Name = base;
				document.Overridden = m_Material->Overrides();
				document.Overridden.Set(MaterialField::Name);
				const std::string text = MaterialIO::SerializeDocument(document);
				const std::filesystem::path absolute = ContentRootPath() / std::filesystem::path(target);
				if (MaterialIO::WriteFileText(absolute.generic_string(), text, &error))
				{
					// 写盘后重新解析(含父级链):面板切开的是**磁盘上的**变体,不是临时实例。
					std::string reloadError;
					if (MaterialLibrary::Get().Reload(target, &reloadError)
						|| MaterialLibrary::Get().Load(target, &reloadError))
						savedPath = MaterialLibrary::NormalizePath(target);
					else
						error = reloadError;
				}
			}
			if (savedPath.empty())
			{
				m_SaveAsFailure = error.empty()
					? Wui::Tr("panel.material.saveas.failed", "Save As failed") : error;
				m_SaveAsFailureFor = target;
				host.Notify(m_SaveAsFailure);
				WLD_CORE_WARN("Save As failed: {0}", m_SaveAsFailure);
			}
			else
			{
				ctx.RecordOp("material", "saveas", savedPath, m_Path);
				// 当前面板切到新材质(标题/路径/校验都跟着新文档走)。
				OpenMaterial(savedPath);
				host.SelectContentAsset(savedPath, "material-saveas");
				m_Status = Wui::Tr("panel.material.status.saved_as", "Saved as ") + savedPath
					+ Wui::Tr("panel.material.status.saved_as.note",
						" (source .wmat unchanged; the variant stores Parent: <this material> and only "
						"the fields this material overrides)");
				m_StatusIsError = false;
				closeRequested = true;
			}
		}
		else if (cancelClicked)
			closeRequested = true;
		else if (escapePressed)
		{
			// Esc 分层:目录下拉展开时先关下拉,第二次才关模态。
			if (folderPopupWasOpen)
				ctx.ClosePopup(folderId);
			else
				closeRequested = true;
		}
		Wui::EndModalFrame(ctx);
		if (closeRequested && ctx.Modal() == modalId)
			CloseSaveAsModal(ctx, host);
	}


	// ---- U25-M2:B 拖放(内容浏览器 ↔ 材质编辑器)----
	//
	// 核心 WUI 的拖拽态是**每个窗口一份**,而内容浏览器(主窗口停靠面板)与材质编辑器
	// (独立窗口/附加标签)不可能同时渲染 —— 所以释放由**源窗口**用全局光标命中这里登记的
	// 落点矩形(屏幕物理像素),投递一次 drop;本面板在渲染时取走。见 Editor::AssetDropBridge。
bool MaterialEditorPanel::PayloadToLogical(const std::string& payload, std::string* logical){
		if (payload.rfind("file:", 0) != 0)
			return false;
		std::string path = payload.substr(5);
		std::replace(path.begin(), path.end(), '\\', '/');
		if (path.empty())
			return false;
		if (logical)
			*logical = path;
		return true;
	}


void MaterialEditorPanel::RegisterSlotDrop(const Wui::WuiContext& ctx, PanelHost& host, const std::string& key, const Wui::WuiRect& rect){
		float originX = 0.0f, originY = 0.0f;
		if (!host.PanelWindowScreenOrigin(ctx, &originX, &originY))
			return;
		const float scale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		Editor::AssetDropBridge::Target target;
		target.Owner = m_PanelId;
		target.Sink = "texture-slot";
		target.Key = key;
		target.PayloadPrefix = "file:";
		target.X = originX + rect.X * scale;
		target.Y = originY + rect.Y * scale;
		target.W = rect.W * scale;
		target.H = rect.H * scale;
		Editor::AssetDropBridge::Get().Register(target);
	}


	// 跨窗口拖放的**交接**:面板只取走一次投放并把逻辑路径转出来(`PayloadToLogical`),值怎么写、
	// 能不能写由调用点(组件返回值 + 写入口)决定 —— 拖放与下拉选择走**同一条**赋值路径。
	// 载荷不可用(拖 .wmat 到槽位 / 不支持的扩展名)在这里给出可读状态行,并算"已消费"。
bool MaterialEditorPanel::TakeSlotDrop(PanelHost& host, const std::string& key, std::string* outLogical){
		(void)host;
		if (outLogical)
			outLogical->clear();
		Editor::AssetDropBridge::Drop drop;
		if (!Editor::AssetDropBridge::Get().TakeDrop(m_PanelId, "texture-slot", key, &drop))
			return false;
		std::string logical;
		if (!PayloadToLogical(drop.Payload, &logical) || !m_Material)
			return false;
		const std::string extension = LowerExtension(logical);
		if (extension == ".wmat")
		{
			// 方案 §B:.wmat 拖到槽位不做特殊处理,只给可读反馈。
			m_Status = Wui::Tr("panel.material.status.drop_wmat_on_slot",
				"This row is a texture slot: drop a texture asset (.wtex) or an image "
				"(.png/.jpg/.jpeg/.tga/.bmp) here, or drop the .wmat on the title to open it");
			m_StatusIsError = true;
			return true;
		}
		// M4-TEX-P6a:资产(`.wtex`)与源图(图片)都是合法落点;写进去的就是拖进来的那个逻辑路径。
		if (!IsTextureRefExtension(extension))
		{
			m_Status = Wui::Tr("panel.material.status.drop_type", "Unsupported drop type: ") + logical;
			m_StatusIsError = true;
			return true;
		}
		const std::string normalized = MaterialLibrary::NormalizePath(logical);
		const std::string& current = ResolvedTextureRefFor(key);
		if (MaterialLibrary::NormalizePath(current) == normalized)
		{
			m_Status = Wui::Tr("panel.material.status.drop_same", "Already uses this texture: ") + logical;
			m_StatusIsError = false;
			return true;
		}
		if (outLogical)
			*outLogical = normalized;
		return true;
	}


std::string MaterialEditorPanel::ResolvedTextureRefFor(const std::string& key) const{
		if (!m_Material)
			return {};
		const MaterialDesc& desc = m_Material->GetDesc();
		if (key == "normal")
			return desc.NormalTexture;
		if (key == "albedo")
			return desc.AlbedoTexture;
		return m_Material->ResolvedParamValue(key);
	}


	// ---- M4-TEX-P11:贴图引用的定位 ----
	//
	// 定位 = 复用宿主既有的"在内容浏览器选中"通道(`PanelHost::SelectContentAsset`);
	// 源图(`.png`)与纹理资产(`.wtex`)走同一条。按钮由 `Wui::WuiTexturePicker` 画
	// (`IdPrefix + ".locate"`),这里只处理它置位的那一次请求。
void MaterialEditorPanel::RevealTextureRef(PanelHost& host, const std::string& logical){
		if (logical.empty())
			return;
		if (host.SelectContentAsset(logical, "material-texture"))
		{
			m_Status = Wui::Tr("panel.material.status.texture_located",
				"Selected in the Content Browser: ") + logical;
			m_StatusIsError = false;
		}
		else
		{
			m_Status = Wui::Tr("panel.material.status.texture_locate_failed",
				"Cannot locate this texture in the Content Browser (missing on disk?): ") + logical;
			m_StatusIsError = true;
		}
	}


void MaterialEditorPanel::RegisterHeaderDrop(const Wui::WuiContext& ctx, PanelHost& host, const Wui::WuiRect& rect){
		float originX = 0.0f, originY = 0.0f;
		if (!host.PanelWindowScreenOrigin(ctx, &originX, &originY))
			return;
		const float scale = Wui::UiScale() > 0.0f ? Wui::UiScale() : 1.0f;
		Editor::AssetDropBridge::Target target;
		target.Owner = m_PanelId;
		target.Sink = "header";
		target.PayloadPrefix = "file:";
		target.X = originX + rect.X * scale;
		target.Y = originY + rect.Y * scale;
		target.W = rect.W * scale;
		target.H = rect.H * scale;
		Editor::AssetDropBridge::Get().Register(target);
	}


bool MaterialEditorPanel::TakeHeaderDrop(Wui::WuiContext& ctx, PanelHost& host){
		Editor::AssetDropBridge::Drop drop;
		if (!Editor::AssetDropBridge::Get().TakeDrop(m_PanelId, "header", std::string(), &drop))
			return false;
		std::string logical;
		if (!PayloadToLogical(drop.Payload, &logical))
			return false;
		if (LowerExtension(logical) != ".wmat")
		{
			m_Status = Wui::Tr("panel.material.status.drop_header_type",
				"The title opens .wmat files: drop the texture on a texture slot row instead");
			m_StatusIsError = true;
			return true;
		}
		RequestOpenMaterial(ctx, logical, host);
		return true;
	}


void MaterialEditorPanel::RequestOpenMaterial(Wui::WuiContext& ctx, const std::string& path, PanelHost& host){
		(void)host;
		const std::string normalized = MaterialLibrary::NormalizePath(path);
		if (!m_Path.empty() && normalized == MaterialLibrary::NormalizePath(m_Path))
		{
			m_Status = Wui::Tr("panel.material.status.already_open",
				"Already open in this window: ") + normalized;
			m_StatusIsError = false;
			return;
		}
		if (m_Material && m_Material->IsDirty())
		{
			// 有未保存改动:先确认(不静默丢弃),确认后走同一条 OpenMaterial。
			m_PendingOpenPath = normalized;
			m_OpenConfirmOpen = true;
			ctx.SetModal(Wui::HashId("material.openconfirm.modal"));
			ctx.RecordOp("material", "open-ask", normalized, m_Path);
			return;
		}
		ctx.RecordOp("material", "open-drop", normalized, m_Path);
		OpenMaterial(normalized);
	}


	// M3:头部的"打开父材质" —— 同窗口切文档,复用 RequestOpenMaterial 的未保存确认路径。
void MaterialEditorPanel::OpenParentMaterial(Wui::WuiContext& ctx, PanelHost& host){
		if (!m_Material)
			return;
		const std::string parent = m_Material->ParentPath();
		if (parent.empty())
		{
			m_Status = Wui::Tr("panel.material.parent.none",
				"Open Parent Material: this material has no parent file (it inherits the engine default)");
			m_StatusIsError = true;
			return;
		}
		RequestOpenMaterial(ctx, parent, host);
	}


void MaterialEditorPanel::DrawOpenConfirmModal(Wui::WuiContext& ctx, PanelHost& host){
		if (!m_OpenConfirmOpen)
			return;
		const Wui::WuiId modalId = Wui::HashId("material.openconfirm.modal");
		const Wui::WuiTheme& theme = host.Theme();
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("panel.material.openconfirm.title", "Open another material?");
		frameDesc.Size = { 480.0f, 190.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
		{
			m_OpenConfirmOpen = false;
			m_PendingOpenPath.clear();
			return;
		}
		Wui::Label(ctx, { frame.X + 16.0f, frame.Y + 52.0f },
			Wui::Tr("panel.material.openconfirm.body", "This window has unsaved changes."), theme.Text, 13.0f);
		Wui::Label(ctx, { frame.X + 16.0f, frame.Y + 74.0f },
			EllipsizeToWidth(ctx, m_PendingOpenPath, frame.W - 32.0f, 12.0f), theme.TextMuted, 12.0f);
		Wui::Label(ctx, { frame.X + 16.0f, frame.Y + 94.0f },
			Wui::Tr("panel.material.openconfirm.body2",
				"Opening it drops those edits (the file on disk is unchanged)."),
			theme.TextMuted, 12.0f);
		const Wui::ModalResult result = Wui::ModalFooter(ctx, frame,
			Wui::Tr("panel.material.openconfirm.discard", "Discard & Open"),
			Wui::Tr("panel.material.openconfirm.cancel", "Cancel"),
			Wui::HashId("material.openconfirm.ok"), Wui::HashId("material.openconfirm.cancel"),
			true, theme);
		const std::string pending = m_PendingOpenPath;
		const bool openPending = result == Wui::ModalResult::Confirm;
		if (openPending || result == Wui::ModalResult::Cancel || escapePressed)
		{
			m_OpenConfirmOpen = false;
			m_PendingOpenPath.clear();
		}
		// 先收 overlay 再清模态态(BeginModalFrame/EndModalFrame 必须成对)。
		Wui::EndModalFrame(ctx);
		if (openPending)
		{
			if (ctx.Modal() == modalId)
				ctx.ClearModal();
			ctx.RecordOp("material", "open-confirmed", pending, m_Path);
			OpenMaterial(pending);
		}
		else if (ctx.Modal() == modalId && !m_OpenConfirmOpen)
			ctx.ClearModal();
	}

}
