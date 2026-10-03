#include "ContentBrowserPanel_Internal.h"

namespace World
{

using namespace ContentBrowserPanelDetail;


	// ---- U25-M2:E 写材质的工作流(新建向导 + 从选中对象提取)----
bool ContentBrowserPanel::OpenNewMaterialWizard(std::string* message){
		m_NewMaterialPendingOpen = true;
		m_NewMaterialPendingFromSelection = false;
		if (message)
			*message = "new-material wizard queued";
		return true;
	}


bool ContentBrowserPanel::OpenNewMaterialFromSelection(std::string* message){
		const Entity selected = m_Host.GetSelectedEntity();
		if (!selected.IsValid())
		{
			if (message)
				*message = "no entity selected (select one in the Scene Hierarchy first)";
			return false;
		}
		m_NewMaterialPendingOpen = true;
		m_NewMaterialPendingFromSelection = true;
		if (message)
			*message = "extract-from-selection wizard queued";
		return true;
	}


std::string ContentBrowserPanel::NewMaterialBaseName() const{
		return AssetBaseName(m_NewMaterialName);
	}


std::string ContentBrowserPanel::NewMaterialTarget() const{
		const std::string name = NewMaterialBaseName();
		std::string folder;
		if (m_NewMaterialFolderIndex >= 0 && m_NewMaterialFolderIndex < static_cast<int>(m_NewMaterialFolders.size()))
			folder = m_NewMaterialFolders[static_cast<size_t>(m_NewMaterialFolderIndex)];
		std::string target = folder.empty() ? std::string() : (folder + "/");
		target += name.empty() ? std::string("(name)") : name;
		target += ".wmat";
		return target;
	}


std::string ContentBrowserPanel::NewMaterialNameError() const{
		const std::string name = NewMaterialBaseName();
		if (name.empty())
			return Wui::Tr("panel.content_browser.new_material.name.empty", "Name cannot be empty");
		if (name == "." || name == "..")
			return Wui::Tr("panel.content_browser.new_material.name.dot", "Name cannot be '.' or '..'");
		for (const char character : name)
			if (character == '\\' || character == '/' || character == ':' || character == '*'
				|| character == '?' || character == '"' || character == '<' || character == '>'
				|| character == '|')
				return Wui::Tr("panel.content_browser.new_material.name.illegal",
					"Name cannot contain \\ / : * ? \" < > |");
		if (name.back() == '.' || name.back() == ' ')
			return Wui::Tr("panel.content_browser.new_material.name.trailing",
				"Name cannot end with a dot or a space");
		return {};
	}


MaterialDesc ContentBrowserPanel::MaterialDescForTemplate(int templateIndex, const std::string& name, const MaterialDesc& seed){
		// seed = Extract 读到的初值(普通新建 = MaterialDesc 默认值)。
		MaterialDesc desc = seed;
		desc.Name = name;
		switch (templateIndex)
		{
			case 1:   // Unlit-ish(近似:自发光 = 基色,金属度/粗糙度归零)
				desc.Emissive = glm::vec3(desc.BaseColor);
				desc.Metallic = 0.0f;
				desc.Roughness = 0.0f;
				break;
			case 2:   // Transparent(= 现有 blend 字段能表达的 alpha 混合)
				desc.BlendMode = MaterialBlendMode::Transparent;
				desc.DoubleSided = false;
				break;
			case 3:   // Additive(近似:Transparent + 自发光 = 基色 x 2;真正的 additive 值属 M3)
				desc.BlendMode = MaterialBlendMode::Transparent;
				desc.DoubleSided = false;
				desc.Emissive = glm::vec3(desc.BaseColor) * 2.0f;
				break;
			default:
				break;
		}
		return desc;
	}


void ContentBrowserPanel::OpenNewMaterialModal(Wui::WuiContext& ctx, bool fromSelection){
		m_NewMaterialOpen = true;
		m_NewMaterialOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		m_NewMaterialFailure.clear();
		m_NewMaterialFailureFor.clear();
		m_NewMaterialTemplate = 0;
		m_NewMaterialSeed = MaterialDesc {};
		m_NewMaterialAssignEntity = Entity {};
		m_NewMaterialFromSelection = fromSelection;
		// M3:Parent 下拉 = 引擎内置默认(下标 0)+ 内容根下所有已有 .wmat。
		m_NewMaterialParentPaths.clear();
		m_NewMaterialParentPaths.push_back(std::string());
		for (const std::string& path : MaterialLibrary::Get().ScanMaterials())
			m_NewMaterialParentPaths.push_back(path);
		m_NewMaterialParentIndex = 0;

		std::string defaultName;
		std::string defaultFolder;
		if (fromSelection)
		{
			// Extract from Selection:初值 = 选中实体 MeshRenderer 的当前材质;
			// 没有材质资产时用实体的 Color 当基色(用户看到的颜色就是初值)。
			Entity selected = m_Host.GetSelectedEntity();
			m_NewMaterialAssignEntity = selected;
			std::string entityName;
			const Ref<Scene> scene = m_Host.GetActiveScene();
			if (selected.IsValid() && scene)
			{
				const Scene& sceneRef = *scene;
				const entt::registry& registry = sceneRef.GetRegistry();
				const entt::entity handle = static_cast<entt::entity>(selected);
				if (const auto* mesh = registry.try_get<MeshRendererComponent>(handle))
				{
					if (!mesh->MaterialPath.empty())
					{
						std::string loadError;
						if (const Ref<Material> source = MaterialLibrary::Get().Load(mesh->MaterialPath, &loadError))
							m_NewMaterialSeed = source->GetDesc();
						defaultFolder = std::filesystem::path(mesh->MaterialPath).parent_path().generic_string();
					}
					else
					{
						m_NewMaterialSeed.BaseColor = mesh->Color;
					}
				}
				if (selected.HasComponent<TagComponent>())
					entityName = selected.GetComponent<TagComponent>().Tag;
			}
			defaultName = SanitizeAssetName(entityName);
			if (defaultName.empty())
				defaultName = "material";
			defaultName += "_material";
		}
		else
		{
			defaultName = "material";
		}
		m_NewMaterialName = defaultName;
		if (defaultFolder.empty())
		{
			// 默认落点 = 内容浏览器当前目录(相对内容根);根目录时退回 materials/。
			std::error_code relativeError;
			const std::filesystem::path relative =
				std::filesystem::relative(m_NewMaterialPendingDir.empty() ? m_Model.Current
					: m_NewMaterialPendingDir, m_Model.Root, relativeError);
			defaultFolder = relativeError || relative.empty() || relative.generic_string() == "."
				? std::string("materials") : relative.generic_string();
		}
		m_NewMaterialFolders = Editor::AssetCatalog::Dirs();
		auto found = std::find(m_NewMaterialFolders.begin(), m_NewMaterialFolders.end(), defaultFolder);
		if (found == m_NewMaterialFolders.end())
		{
			m_NewMaterialFolders.push_back(defaultFolder);
			std::sort(m_NewMaterialFolders.begin(), m_NewMaterialFolders.end());
			found = std::find(m_NewMaterialFolders.begin(), m_NewMaterialFolders.end(), defaultFolder);
		}
		m_NewMaterialFolderIndex = found == m_NewMaterialFolders.end()
			? 0 : static_cast<int>(found - m_NewMaterialFolders.begin());
		ctx.SetModal(Wui::HashId("material.new.modal"));
		m_Host.SetPanelModalOwner(Id());
		ctx.SetFocus(Wui::HashId("material.new.name"));
		ctx.RecordOp("browser", fromSelection ? "new-material-extract-ask" : "new-material-ask",
			m_NewMaterialName, defaultFolder);
	}


void ContentBrowserPanel::CloseNewMaterialModal(Wui::WuiContext& ctx){
		m_NewMaterialOpen = false;
		m_NewMaterialFromSelection = false;
		m_NewMaterialFolders.clear();
		m_NewMaterialFolderIndex = 0;
		m_NewMaterialFailure.clear();
		m_NewMaterialFailureFor.clear();
		m_NewMaterialSeed = MaterialDesc {};
		m_NewMaterialAssignEntity = Entity {};
		m_NewMaterialPendingDir.clear();
		if (ctx.Modal() == Wui::HashId("material.new.modal"))
			ctx.ClearModal();
		ctx.ClosePopup(Wui::HashId("material.new.folder"));
		ctx.ClosePopup(Wui::HashId("material.new.template"));
		m_Host.SetPanelModalOwner(std::string());
	}


void ContentBrowserPanel::DrawNewMaterialModal(Wui::WuiContext& ctx){
		if (!m_NewMaterialOpen)
			return;
		const Wui::WuiId modalId = Wui::HashId("material.new.modal");
		const Wui::WuiTheme& theme = m_Host.Theme();
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = m_NewMaterialFromSelection
			? Wui::Tr("panel.content_browser.new_material.title.extract", "Extract Material from Selection")
			: Wui::Tr("panel.content_browser.new_material.title", "New Material");
		// M3:向导多了一行 Parent(选父级 + 起始覆盖),模态相应加高。
		frameDesc.Size = { 620.0f, 470.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
		{
			CloseNewMaterialModal(ctx);
			return;
		}
		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 130.0f;
		const float fieldW = frame.W - 146.0f - 68.0f;
		const Wui::WuiId templateId = Wui::HashId("material.new.template");
		const Wui::WuiId parentId = Wui::HashId("material.new.parent");
		const Wui::WuiId nameId = Wui::HashId("material.new.name");
		const Wui::WuiId folderId = Wui::HashId("material.new.folder");
		const bool templatePopupWasOpen = ctx.IsPopupOpen(templateId);
		const bool parentPopupWasOpen = ctx.IsPopupOpen(parentId);
		const bool folderPopupWasOpen = ctx.IsPopupOpen(folderId);
		const bool justOpened = ctx.Frame() == m_NewMaterialOpenedFrame;

		// ---- 起始覆盖下拉(Standard / Unlit-ish / Transparent / Additive)----
		// M3:这一栏不再假装是"着色模型":它描述的是**这次新建会写下哪些覆盖字段**
		// (父级是引擎默认时 = 自包含材质;父级是 .wmat 时 = 相对父级的起始覆盖)。
		float cursorY = frame.Y + 46.0f;
		const std::string templateLabel = Wui::Tr("panel.content_browser.new_material.template",
			"Starting overrides");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, templateLabel, theme.TextMuted, 13.0f);
		std::vector<std::string> templateOptions;
		for (const NewMaterialTemplate& entry : kNewMaterialTemplates)
			templateOptions.push_back(Wui::Tr(entry.LabelKey, entry.LabelEn));
		const Wui::WuiRect templateRect { fieldX, cursorY, fieldW, 24.0f };
		Wui::Combo(ctx, templateId, templateRect, templateLabel, templateOptions, m_NewMaterialTemplate, theme);
		const int templateIndex = std::clamp(m_NewMaterialTemplate, 0, kNewMaterialTemplateCount - 1);
		const std::string templateDoc = Wui::Tr(kNewMaterialTemplates[templateIndex].DocKey,
			kNewMaterialTemplates[templateIndex].DocEn);
		cursorY += 30.0f;
		const std::pair<std::string, std::string> docLines =
			WrapTwoLines(ctx, templateDoc, frame.W - 32.0f, 12.0f);
		Wui::Label(ctx, { labelX, cursorY }, docLines.first, theme.TextMuted, 12.0f);
		if (!docLines.second.empty())
			Wui::Label(ctx, { labelX, cursorY + 15.0f }, docLines.second, theme.TextMuted, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.new.template.doc");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = templateLabel;
			node.Value = templateDoc;
			node.Tooltip = templateDoc;
			node.Rect = { labelX, cursorY, frame.W - 32.0f, docLines.second.empty() ? 16.0f : 31.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += docLines.second.empty() ? 24.0f : 39.0f;

		// M3:起始覆盖 + 名称 + 目录三行之后,与底部 Parent 行之间始终留出两行说明的高度 ——
		// 这样"起始覆盖"下拉的选项条目(弹层从它下方展开)不会压到"目录"触发条上:
		// WUI 的 popup 只在**下一帧**遮挡下层控件,点选项那一下的 release 会落到条目正下方的
		// 控件上(实测)。留白是这条规则下的必要布局约束,不是随手加的空行。
		cursorY += 39.0f;

		// ---- 名称(后缀自动补)----
		const std::string nameLabel = Wui::Tr("panel.content_browser.new_material.name", "Name");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, nameLabel, theme.TextMuted, 13.0f);
		const Wui::WuiRect nameRect { fieldX, cursorY, fieldW, 24.0f };
		const bool nameFocused = ctx.Focus() == nameId;
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = nameLabel;
		nameA11y.Placeholder = Wui::Tr("panel.content_browser.new_material.name.placeholder", "Material name");
		Wui::TextField(ctx, nameId, nameRect, m_NewMaterialName, theme, nullptr, &nameA11y);
		const bool nameSubmitted = nameFocused && ctx.IsKeyPressed(KeyCodes::Enter);
		Wui::Label(ctx, { nameRect.X + nameRect.W + 8.0f, cursorY + 6.0f }, ".wmat", theme.TextMuted, 13.0f);
		const std::string nameError = NewMaterialNameError();
		if (!nameError.empty())
			Wui::Label(ctx, { fieldX, cursorY + 27.0f }, nameError, theme.Danger, 12.0f);
		cursorY += 46.0f;

		// ---- 目录(内容根下的目录,可搜索)----
		const std::string folderLabel = Wui::Tr("panel.content_browser.new_material.folder", "Folder");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, folderLabel, theme.TextMuted, 13.0f);
		const Wui::WuiRect folderRect { fieldX, cursorY, fieldW, 24.0f };
		Wui::SearchableCombo(ctx, folderId, folderRect, folderLabel, m_NewMaterialFolders,
			m_NewMaterialFolderIndex, theme);
		const std::string folderText = (m_NewMaterialFolderIndex >= 0
			&& m_NewMaterialFolderIndex < static_cast<int>(m_NewMaterialFolders.size()))
			? m_NewMaterialFolders[static_cast<size_t>(m_NewMaterialFolderIndex)] : std::string();
		{
			Wui::WuiAccessNode node;
			node.Id = folderId;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "search-combo";
			node.Label = folderLabel;
			node.Value = folderText;
			node.Tooltip = Wui::Tr("panel.content_browser.new_material.folder.tooltip",
				"Folder under the content root (searchable); a missing folder is created on confirm.");
			node.Rect = folderRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		const std::filesystem::path folderAbsolute = m_Model.Root / std::filesystem::path(folderText);
		if (!folderText.empty() && !std::filesystem::is_directory(folderAbsolute))
			Wui::Label(ctx, { fieldX, cursorY + 27.0f },
				Wui::Tr("panel.content_browser.new_material.folder.note",
					"Folder does not exist yet — it will be created"), theme.Warning, 12.0f);
		cursorY += 46.0f;

		// ---- 实时落点回显 + 覆盖警告 ----
		const std::string target = NewMaterialTarget();
		if (!m_NewMaterialFailure.empty() && m_NewMaterialFailureFor != target)
		{
			m_NewMaterialFailure.clear();
			m_NewMaterialFailureFor.clear();
		}
		const std::string previewLabel = Wui::Tr("panel.content_browser.new_material.preview.label", "Will create");
		Wui::Label(ctx, { labelX, cursorY + 3.0f }, previewLabel, theme.TextMuted, 12.0f);
		Wui::Label(ctx, { fieldX, cursorY + 1.0f }, target, theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.new.preview");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = previewLabel;
			node.Value = target;
			node.Tooltip = Wui::Tr("panel.content_browser.new_material.preview.tooltip",
				"Logical path of the file that will be written (folder + name + .wmat).");
			node.Rect = { fieldX, cursorY - 3.0f, fieldW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 24.0f;
		std::error_code existsError;
		const bool targetExists = nameError.empty()
			&& std::filesystem::exists(m_Model.Root / std::filesystem::path(target), existsError);
		std::string warningText;
		if (!m_NewMaterialFailure.empty())
			warningText = m_NewMaterialFailure;
		else if (targetExists)
			warningText = Wui::Tr("panel.content_browser.new_material.exists",
				"Already exists — overwriting: ") + target;
		if (!warningText.empty())
			Wui::Label(ctx, { fieldX, cursorY }, warningText, theme.Danger, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.new.warning");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.content_browser.new_material.warning.label", "Warning");
			node.Value = warningText;
			node.Tooltip = Wui::Tr("panel.content_browser.new_material.warning.tooltip",
				"Red line = the target exists and would be overwritten; empty = no conflict.");
			node.Rect = { fieldX, cursorY - 4.0f, fieldW, 18.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// ---- M3:Parent(父级)—— 默认引擎内置默认;可选内容根下任意 .wmat ----
		// 位置 = 内容最下方、按钮条正上方,而且**收窄宽度**:
		//  - 它的弹层从下方展开(条目多、可以很长),下面只剩空白与按钮条左侧的空区;
		//    收窄后弹层 x 范围与 OK/Cancel 不重叠 —— 点条目那一下的 release 不会顺手按到按钮
		//    (WUI 的 popup 只在下一帧遮挡下层控件,选项 release 会落到条目正下方的控件上);
		//  - 上面的"起始覆盖/目录"弹层不会向下延伸到这里(见前面的留白注释)。
		const float footerY = frame.Y + frame.H - Wui::ModalFooterPadding - Wui::ModalFooterHeight;
		const std::string parentLabel = Wui::Tr("panel.content_browser.new_material.parent", "Parent");
		std::vector<std::string> parentOptions;
		parentOptions.push_back(Wui::Tr("panel.content_browser.new_material.parent.engine",
			"Engine Default"));
		for (size_t index = 1; index < m_NewMaterialParentPaths.size(); ++index)
			parentOptions.push_back(m_NewMaterialParentPaths[index]);
		m_NewMaterialParentIndex = std::clamp(m_NewMaterialParentIndex, 0,
			static_cast<int>(parentOptions.size()) - 1);
		const std::string parentPath = m_NewMaterialParentIndex > 0
			? parentOptions[static_cast<size_t>(m_NewMaterialParentIndex)] : std::string();
		const std::string parentDoc = parentPath.empty()
			? Wui::Tr("panel.content_browser.new_material.parent.engine.doc",
				"Engine Default: the new material is self-contained — every field is written, "
				"exactly like a material created before M3.")
			: Wui::Tr("panel.content_browser.new_material.parent.file.doc",
				"Inherit from:") + " " + parentPath
				+ Wui::Tr("panel.content_browser.new_material.parent.file.doc2",
					" — the file stores Parent plus the starting overrides below; everything else "
					"keeps following the parent (edit it later in the material editor).");
		const Wui::WuiRect parentRect { fieldX, footerY - 30.0f, std::min(fieldW, 300.0f), 24.0f };
		Wui::Label(ctx, { labelX, parentRect.Y + 5.0f }, parentLabel, theme.TextMuted, 13.0f);
		Wui::Combo(ctx, parentId, parentRect, parentLabel, parentOptions,
			m_NewMaterialParentIndex, theme);
		const Wui::WuiRect parentHint { labelX, parentRect.Y - 18.0f,
			std::max(60.0f, frame.W - 32.0f), 16.0f };
		Wui::Label(ctx, { parentHint.X, parentHint.Y },
			EllipsizeToWidth(ctx, parentDoc, parentHint.W, 12.0f), theme.TextMuted, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("material.new.parent.doc");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = parentLabel;
			node.Value = parentDoc;
			node.Tooltip = parentDoc;
			node.Rect = parentHint;
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		Wui::Tooltip(ctx, parentRect, parentDoc);

		// ---- 底部按钮条 ----
		const bool canCreate = nameError.empty();
		const Wui::WuiRect okRect { frame.X + frame.W - 16.0f - 150.0f, footerY, 150.0f,
			Wui::ModalFooterHeight };
		const Wui::WuiRect cancelRect { okRect.X - 8.0f - 96.0f, footerY, 96.0f, Wui::ModalFooterHeight };
		const std::string okLabel = targetExists
			? Wui::Tr("panel.content_browser.new_material.overwrite", "Overwrite")
			: Wui::Tr("panel.content_browser.new_material.create", "Create Material");
		const std::string okTooltip = !canCreate
			? (nameError + " — " + Wui::Tr("panel.content_browser.new_material.ok.disabled",
				"fix the name to enable this button"))
			: (targetExists
				? Wui::Tr("panel.content_browser.new_material.overwrite.tooltip",
					"The file exists: overwrite it with this template")
				: (m_NewMaterialFromSelection
					? Wui::Tr("panel.content_browser.new_material.create.extract.tooltip",
						"Write the .wmat, assign it back to the selected entity and open it in the material editor")
					: Wui::Tr("panel.content_browser.new_material.create.tooltip",
						"Write the .wmat and open it in the material editor")));
		const bool okClicked = ModalActionButton(ctx, Wui::HashId("material.new.ok"), okRect, okLabel,
			okTooltip, canCreate, true, theme);
		const bool cancelClicked = ModalActionButton(ctx, Wui::HashId("material.new.cancel"), cancelRect,
			Wui::Tr("panel.content_browser.new_material.cancel", "Cancel"),
			Wui::Tr("panel.content_browser.new_material.cancel.tooltip",
				"Close without writing anything (Esc)"),
			true, false, theme);
		bool closeRequested = false;
		if ((okClicked || (nameSubmitted && !justOpened)) && canCreate)
		{
			const std::string absolute = (m_Model.Root / std::filesystem::path(target)).generic_string();
			const std::string base = NewMaterialBaseName();
			std::string error;
			bool wrote = false;
			if (parentPath.empty())
			{
				// 父级 = 引擎内置默认:自包含材质(全字段、没有 Parent 行 → 仍是 v1 写法,
				// 与 M3 前新建的 .wmat 逐字节一致)。
				const MaterialDesc desc = MaterialDescForTemplate(templateIndex, base, m_NewMaterialSeed);
				// 绝对路径:MaterialIO 只认"内容根/<path>",相对路径不保证落在内容根里(CreateMaterialAsset 记的坑)。
				wrote = MaterialIO::WriteFileText(absolute, MaterialIO::Serialize(desc), &error);
			}
			else
			{
				// 父级 = 已有 .wmat:真材质实例 —— 只写 Parent + 本次的起始覆盖。
				Ref<Material> instance = MaterialLibrary::Get().CreateInstance(parentPath, base, &error);
				if (instance)
				{
					// 起始覆盖按预设落到具体字段(相对父级的当前解析值):
					// Standard = 不改任何字段(纯继承),其余三个只改它们真正涉及的那几项。
					const MaterialDesc resolved = instance->GetDesc();
					switch (templateIndex)
					{
						case 1:   // Unlit-ish:自发光 = 基色,金属度/粗糙度归零
							instance->SetEmissive(glm::vec3(resolved.BaseColor));
							instance->SetMetallic(0.0f);
							instance->SetRoughness(0.0f);
							break;
						case 2:   // Transparent:混合模式
							instance->SetBlendMode(MaterialBlendMode::Transparent);
							break;
						case 3:   // Additive(近似):透明 + 自发光 = 基色 x 2
							instance->SetBlendMode(MaterialBlendMode::Transparent);
							instance->SetEmissive(glm::vec3(resolved.BaseColor) * 2.0f);
							break;
						default:
							break;
					}
					wrote = MaterialLibrary::Get().Save(instance, target, &error);
				}
			}
			if (!wrote)
			{
				m_NewMaterialFailure = error.empty() ? std::string("could not write the .wmat") : error;
				m_NewMaterialFailureFor = target;
				NotifyAssetFailure(m_NewMaterialFailure);
				WLD_CORE_WARN("New material failed: {0}", m_NewMaterialFailure);
			}
			else
			{
				WLD_CORE_INFO("[material-ui] created {0} (template {1})", target, templateIndex);
				// 内容浏览器选中新资产(与"新建资产"同一条通道)+ 在材质编辑器里打开它。
				SelectCreated(m_Model.Root / std::filesystem::path(target), "new-material");
				m_Host.OpenMaterialEditor(target);
				std::string notice = Wui::Tr("panel.content_browser.new_material.created", "Created ")
					+ target;
				if (m_NewMaterialFromSelection && m_NewMaterialAssignEntity.IsValid())
				{
					std::string assignMessage;
					if (m_Host.SetEntityMaterialPath(m_NewMaterialAssignEntity, target, &assignMessage))
						notice += " · " + Wui::Tr("panel.content_browser.new_material.assigned",
							"assigned back to the selected entity");
					else
						notice += " · " + Wui::Tr("panel.content_browser.new_material.assign_failed",
							"could not assign it back: ") + assignMessage;
				}
				m_Host.Notify(notice);
				closeRequested = true;
			}
		}
		else if (cancelClicked)
			closeRequested = true;
		else if (escapePressed)
		{
			// Esc 分层:先关展开的下拉,第二次才关模态。
			if (templatePopupWasOpen)
				ctx.ClosePopup(templateId);
			else if (parentPopupWasOpen)
				ctx.ClosePopup(parentId);
			else if (folderPopupWasOpen)
				ctx.ClosePopup(folderId);
			else
				closeRequested = true;
		}
		// 先收 overlay 再清模态态(BeginModalFrame/EndModalFrame 必须成对)。
		Wui::EndModalFrame(ctx);
		if (closeRequested && ctx.Modal() == modalId)
			CloseNewMaterialModal(ctx);
	}


	// ==== M4-S2/Slang-B1:Material Shader(`.slang`)新建向导 ====
	// 与"新建材质"同一套 U13d 交互与同一套模态骨架(名称 + 目录 + 实时落点 + 覆盖警告 + Enter/Esc);
	// 差别只有两点:扩展名是 `.slang`、内容是一份**可编译的起始代码**(不是字段预设)且带契约注释头。
bool ContentBrowserPanel::OpenNewShaderWizard(std::string* message){
		m_NewShaderPendingOpen = true;
		if (message)
			*message = "new material shader wizard queued";
		return true;
	}


	// Slang-B1:新建 `.slang` 的**注释头**(两条模板共用)—— 把三条硬规则与契约文档指针写进
	// 文件本身,而不是只留在文档里:① 严格类型;② 贴图参数 = 组合采样器;③ 资源显式 binding。
	// 验收(plan B1-5):这段文本必须含 `Sampler2D`、`.Sample(uv)` 与严格类型提示。
std::string ContentBrowserPanel::ShaderContractHeader(){
		std::string header;
		header += "// Slang material shader (.slang) -- HLSL syntax with Slang strict rules.\n";
		header += "//\n";
		header += "// Hard rules (the old lax HLSL forms are errors now):\n";
		header += "//   1. Strict types: no implicit width conversion.\n";
		header += "//      `float3 v = color4;` is an error -> truncate explicitly: `float3 v = color4.xyz;`\n";
		header += "//      (`float3v += color4;` is an error too -> `float3v += color4.xyz;`).\n";
		header += "//   2. A `//! param Texture2D` declaration becomes a combined sampler `Sampler2D <name>`;\n";
		header += "//      sample it as `Albedo.Sample(uv)` (uv = input.UV) -- there is no sampler argument.\n";
		header += "//   3. Resources use explicit `[[vk::binding(N, S)]]`; `register(...)` is not supported.\n";
		header += "//\n";
		header += "// Contract: docs/dev/shader-contract.md\n";
		header += "\n";
		return header;
	}


std::string ContentBrowserPanel::ShaderTemplateSource(int templateIndex){
		// MAT-FN4:材质函数(库文件)= 纯函数模板 —— 没有 Evaluate 入口,也没有 //! param。
		if (templateIndex == kNewShaderLibraryTemplate)
		{
			std::string source;
			source += "// Material function library (.slang) -- reusable pure functions.\n";
			source += "//\n";
			source += "// Use it from a material (the include path is relative to assets/shaders):\n";
			source += "//     #include \"lib/<this file name>.slang\"\n";
			source += "// then call the functions below like any other function.\n";
			source += "//\n";
			source += "// Rules (contract: docs/dev/shader-contract.md §9):\n";
			source += "//   * pure functions only -- no Evaluate() entry, no `//! param` annotations,\n";
			source += "//     no material `input` / `surface` access;\n";
			source += "//   * no Texture2D / Sampler2D declarations -- take a Sampler2D parameter if you\n";
			source += "//     need to sample (the material owns the texture slot);\n";
			source += "//   * strict Slang types (no implicit width conversion); trailing parameters may\n";
			source += "//     carry defaults, written with explicit types (float2(0.5, 0.5)).\n";
			source += "\n";
			source += "// Smallest working example -- rename it and add your own functions below.\n";
			source += "float3 WeTint(float3 baseColor, float3 tint, float amount = 1.0)\n";
			source += "{\n";
			source += "    return lerp(baseColor, baseColor * tint, saturate(amount));\n";
			source += "}\n";
			return source;
		}
		// 起始代码来自内核(M4-S1 的 MaterialSurfaceCompiler):契约字段有默认值,空改动也能编译。
		// 引擎不在这里留副本 —— 内核改骨架,新建的文件跟着变。
		std::string source = MaterialSurfaceCompiler::DefaultSurfaceFunctionSource();
		// Slang-B1:两条模板都带契约注释头(新建文件的第一眼 = 三条硬规则 + 契约文档指针)。
		std::string header = ShaderContractHeader();
		if (templateIndex <= 0)
			return header + source;
		header += "// Material parameters are declared in the file itself; the editor reads these\n";
		header += "// annotations to build the parameter panel (values default from the shader).\n";
		header += "// Syntax: //! param <type> <name> = <default> [min,max] unit(\"x\") group(\"G\") label(\"L\")\n";
		header += "// Types (case sensitive): Float Vec2 Vec3 Vec4 Color Int Bool Texture2D.\n";
		header += "// [min,max] only applies to Float / Int; values use the material value dialect\n";
		header += "// (0.25 / 1, 0.5 / true / textures/icon.png).\n";
		header += "\n";
		header += "//! param Float     roughness   = 0.35 [0.02,1] group(\"Surface\") label(\"Roughness\")\n";
		header += "//! param Color     baseColor   = 0.85, 0.72, 0.55, 1 group(\"Surface\") label(\"Base Colour\")\n";
		header += "//! param Texture2D albedo      = \"\" group(\"Maps\") label(\"Albedo\")\n";
		header += "//! param Bool      doubleSided = false group(\"Geometry\") label(\"Double Sided\")\n";
		header += "//! param Int       uvChannel   = 0 [0,3] group(\"Maps\") label(\"UV Channel\")\n";
		header += "\n";
		return header + source;
	}


std::string ContentBrowserPanel::NewShaderTarget() const{
		const std::string name = ShaderBaseName(m_NewShaderName);
		std::string folder;
		if (m_NewShaderFolderIndex >= 0
			&& m_NewShaderFolderIndex < static_cast<int>(m_NewShaderFolders.size()))
			folder = m_NewShaderFolders[static_cast<size_t>(m_NewShaderFolderIndex)];
		std::string target = folder.empty() ? std::string() : (folder + "/");
		target += name.empty() ? std::string("(name)") : name;
		target += ".slang";
		return target;
	}


std::string ContentBrowserPanel::NewShaderNameError() const{
		// 与材质向导同一套命名规则(非法字符 / 空名 / 结尾点或空格)。
		const std::string name = ShaderBaseName(m_NewShaderName);
		if (name.empty())
			return Wui::Tr("panel.content_browser.new_shader.name.empty", "Name cannot be empty");
		if (name == "." || name == "..")
			return Wui::Tr("panel.content_browser.new_shader.name.dot", "Name cannot be '.' or '..'");
		for (const char character : name)
			if (character == '\\' || character == '/' || character == ':' || character == '*'
				|| character == '?' || character == '"' || character == '<' || character == '>'
				|| character == '|')
				return Wui::Tr("panel.content_browser.new_shader.name.illegal",
					"Name cannot contain \\ / : * ? \" < > |");
		if (name.back() == '.' || name.back() == ' ')
			return Wui::Tr("panel.content_browser.new_shader.name.trailing",
				"Name cannot end with a dot or a space");
		return {};
	}


void ContentBrowserPanel::OpenNewShaderModal(Wui::WuiContext& ctx){
		m_NewShaderOpen = true;
		m_NewShaderOpenedFrame = static_cast<uint32_t>(ctx.Frame());
		m_NewShaderFailure.clear();
		m_NewShaderFailureFor.clear();
		m_NewShaderTemplate = 0;
		m_NewShaderName = "material_shader";
		// 默认落点 = 内容浏览器当前目录(相对内容根);根目录时退回 shaders/。
		std::error_code relativeError;
		const std::filesystem::path relative =
			std::filesystem::relative(m_NewShaderPendingDir.empty() ? m_Model.Current
				: m_NewShaderPendingDir, m_Model.Root, relativeError);
		std::string defaultFolder = relativeError || relative.empty() || relative.generic_string() == "."
			? std::string("shaders") : relative.generic_string();
		m_NewShaderFolders = Editor::AssetCatalog::Dirs();
		auto found = std::find(m_NewShaderFolders.begin(), m_NewShaderFolders.end(), defaultFolder);
		if (found == m_NewShaderFolders.end())
		{
			m_NewShaderFolders.push_back(defaultFolder);
			std::sort(m_NewShaderFolders.begin(), m_NewShaderFolders.end());
			found = std::find(m_NewShaderFolders.begin(), m_NewShaderFolders.end(), defaultFolder);
		}
		m_NewShaderFolderIndex = found == m_NewShaderFolders.end()
			? 0 : static_cast<int>(found - m_NewShaderFolders.begin());
		ctx.SetModal(Wui::HashId("shader.new.modal"));
		m_Host.SetPanelModalOwner(Id());
		ctx.SetFocus(Wui::HashId("shader.new.name"));
		ctx.RecordOp("browser", "new-shader-ask", m_NewShaderName, defaultFolder);
	}


void ContentBrowserPanel::CloseNewShaderModal(Wui::WuiContext& ctx){
		m_NewShaderOpen = false;
		m_NewShaderFolders.clear();
		m_NewShaderFailure.clear();
		m_NewShaderFailureFor.clear();
		if (ctx.Modal() == Wui::HashId("shader.new.modal"))
			ctx.ClearModal();
		ctx.ClosePopup(Wui::HashId("shader.new.folder"));
		ctx.ClosePopup(Wui::HashId("shader.new.template"));
		m_Host.SetPanelModalOwner(std::string());
	}


void ContentBrowserPanel::DrawNewShaderModal(Wui::WuiContext& ctx){
		if (!m_NewShaderOpen)
			return;
		const Wui::WuiId modalId = Wui::HashId("shader.new.modal");
		const Wui::WuiTheme& theme = m_Host.Theme();
		Wui::ModalFrameDesc frameDesc;
		frameDesc.Id = modalId;
		frameDesc.Title = Wui::Tr("panel.content_browser.new_shader.title", "New Material Shader");
		frameDesc.Size = { 620.0f, 390.0f };
		Wui::WuiRect frame;
		bool escapePressed = false;
		if (!Wui::BeginModalFrame(ctx, frameDesc, &frame, &escapePressed, theme))
		{
			CloseNewShaderModal(ctx);
			return;
		}
		const float labelX = frame.X + 16.0f;
		const float fieldX = frame.X + 130.0f;
		const float fieldW = frame.W - 146.0f - 68.0f;
		const Wui::WuiId templateId = Wui::HashId("shader.new.template");
		const Wui::WuiId nameId = Wui::HashId("shader.new.name");
		const Wui::WuiId folderId = Wui::HashId("shader.new.folder");
		const bool templatePopupWasOpen = ctx.IsPopupOpen(templateId);
		const bool folderPopupWasOpen = ctx.IsPopupOpen(folderId);
		const bool justOpened = ctx.Frame() == m_NewShaderOpenedFrame;

		// ---- 起始代码(模板)----
		float cursorY = frame.Y + 46.0f;
		const std::string templateLabel = Wui::Tr("panel.content_browser.new_shader.template", "Starting code");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, templateLabel, theme.TextMuted, 13.0f);
		std::vector<std::string> templateOptions;
		for (const NewShaderTemplate& entry : kNewShaderTemplates)
			templateOptions.push_back(Wui::Tr(entry.LabelKey, entry.LabelEn));
		const Wui::WuiRect templateRect { fieldX, cursorY, fieldW, 24.0f };
		const int templateBefore = m_NewShaderTemplate;
		Wui::Combo(ctx, templateId, templateRect, templateLabel, templateOptions, m_NewShaderTemplate, theme);
		const int templateIndex = std::clamp(m_NewShaderTemplate, 0, kNewShaderTemplateCount - 1);
		// MAT-FN4:切到"材质函数(库文件)"时把落点默认到 assets/shaders/lib(不存在也没关系:
		// 创建时会建目录),名称默认值也换成 my_function(只在用户没改过默认名时替换)。
		if (templateIndex != templateBefore && templateIndex == kNewShaderLibraryTemplate)
		{
			auto library = std::find(m_NewShaderFolders.begin(), m_NewShaderFolders.end(),
				std::string("shaders/lib"));
			if (library == m_NewShaderFolders.end())
			{
				m_NewShaderFolders.push_back("shaders/lib");
				std::sort(m_NewShaderFolders.begin(), m_NewShaderFolders.end());
				library = std::find(m_NewShaderFolders.begin(), m_NewShaderFolders.end(),
					std::string("shaders/lib"));
			}
			if (library != m_NewShaderFolders.end())
				m_NewShaderFolderIndex = static_cast<int>(library - m_NewShaderFolders.begin());
			if (m_NewShaderName == "material_shader")
				m_NewShaderName = "my_function";
		}
		const std::string templateDoc = Wui::Tr(kNewShaderTemplates[templateIndex].DocKey,
			kNewShaderTemplates[templateIndex].DocEn);
		cursorY += 30.0f;
		const std::pair<std::string, std::string> docLines =
			WrapTwoLines(ctx, templateDoc, frame.W - 32.0f, 12.0f);
		Wui::Label(ctx, { labelX, cursorY }, docLines.first, theme.TextMuted, 12.0f);
		if (!docLines.second.empty())
			Wui::Label(ctx, { labelX, cursorY + 15.0f }, docLines.second, theme.TextMuted, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("shader.new.template.doc");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = templateLabel;
			node.Value = templateDoc;
			node.Tooltip = templateDoc;
			node.Rect = { labelX, cursorY, frame.W - 32.0f, docLines.second.empty() ? 16.0f : 31.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += docLines.second.empty() ? 24.0f : 39.0f;
		// 与材质向导同一条留白约束:模板下拉的选项弹层从下方展开,点条目的 release 不能落到
		// 正下方的控件上(下一帧的遮挡才生效),所以这里固定留出两行高度。
		cursorY += 39.0f;

		// ---- 名称(后缀固定 .slang)----
		const std::string nameLabel = Wui::Tr("panel.content_browser.new_shader.name", "Name");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, nameLabel, theme.TextMuted, 13.0f);
		const Wui::WuiRect nameRect { fieldX, cursorY, fieldW, 24.0f };
		const bool nameFocused = ctx.Focus() == nameId;
		Wui::TextFieldA11y nameA11y;
		nameA11y.Label = nameLabel;
		nameA11y.Placeholder = Wui::Tr("panel.content_browser.new_shader.name.placeholder", "Shader name");
		Wui::TextField(ctx, nameId, nameRect, m_NewShaderName, theme, nullptr, &nameA11y);
		const bool nameSubmitted = nameFocused && ctx.IsKeyPressed(KeyCodes::Enter);
		Wui::Label(ctx, { nameRect.X + nameRect.W + 8.0f, cursorY + 6.0f }, ".slang", theme.TextMuted, 13.0f);
		const std::string nameError = NewShaderNameError();
		if (!nameError.empty())
			Wui::Label(ctx, { fieldX, cursorY + 27.0f }, nameError, theme.Danger, 12.0f);
		cursorY += 46.0f;

		// ---- 目录(内容根下的目录,可搜索)----
		const std::string folderLabel = Wui::Tr("panel.content_browser.new_shader.folder", "Folder");
		Wui::Label(ctx, { labelX, cursorY + 5.0f }, folderLabel, theme.TextMuted, 13.0f);
		const Wui::WuiRect folderRect { fieldX, cursorY, fieldW, 24.0f };
		Wui::SearchableCombo(ctx, folderId, folderRect, folderLabel, m_NewShaderFolders,
			m_NewShaderFolderIndex, theme);
		const std::string folderText = (m_NewShaderFolderIndex >= 0
			&& m_NewShaderFolderIndex < static_cast<int>(m_NewShaderFolders.size()))
			? m_NewShaderFolders[static_cast<size_t>(m_NewShaderFolderIndex)] : std::string();
		{
			Wui::WuiAccessNode node;
			node.Id = folderId;
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "search-combo";
			node.Label = folderLabel;
			node.Value = folderText;
			node.Tooltip = Wui::Tr("panel.content_browser.new_shader.folder.tooltip",
				"Folder under the content root (searchable); a missing folder is created on confirm.");
			node.Rect = folderRect;
			node.Enabled = true;
			node.Interactive = true;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		const std::filesystem::path folderAbsolute = m_Model.Root / std::filesystem::path(folderText);
		if (!folderText.empty() && !std::filesystem::is_directory(folderAbsolute))
			Wui::Label(ctx, { fieldX, cursorY + 27.0f },
				Wui::Tr("panel.content_browser.new_shader.folder.note",
					"Folder does not exist yet — it will be created"), theme.Warning, 12.0f);
		cursorY += 46.0f;

		// ---- 实时落点回显 + 覆盖警告 ----
		const std::string target = NewShaderTarget();
		if (!m_NewShaderFailure.empty() && m_NewShaderFailureFor != target)
		{
			m_NewShaderFailure.clear();
			m_NewShaderFailureFor.clear();
		}
		const std::string previewLabel = Wui::Tr("panel.content_browser.new_shader.preview.label", "Will create");
		Wui::Label(ctx, { labelX, cursorY + 3.0f }, previewLabel, theme.TextMuted, 12.0f);
		Wui::Label(ctx, { fieldX, cursorY + 1.0f }, target, theme.Text, 13.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("shader.new.preview");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = previewLabel;
			node.Value = target;
			node.Tooltip = Wui::Tr("panel.content_browser.new_shader.preview.tooltip",
				"Logical path of the file that will be written (folder + name + .slang).");
			node.Rect = { fieldX, cursorY - 3.0f, fieldW, 20.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}
		cursorY += 24.0f;
		std::error_code existsError;
		const bool targetExists = nameError.empty()
			&& std::filesystem::exists(m_Model.Root / std::filesystem::path(target), existsError);
		std::string warningText;
		if (!m_NewShaderFailure.empty())
			warningText = m_NewShaderFailure;
		else if (targetExists)
			warningText = Wui::Tr("panel.content_browser.new_shader.exists",
				"Already exists — overwriting: ") + target;
		if (!warningText.empty())
			Wui::Label(ctx, { fieldX, cursorY }, warningText, theme.Danger, 12.0f);
		{
			Wui::WuiAccessNode node;
			node.Id = Wui::HashId("shader.new.warning");
			node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
			node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
			node.Kind = "text";
			node.Label = Wui::Tr("panel.content_browser.new_shader.warning.label", "Warning");
			node.Value = warningText;
			node.Tooltip = Wui::Tr("panel.content_browser.new_shader.warning.tooltip",
				"Red line = the target exists and would be overwritten; empty = no conflict.");
			node.Rect = { fieldX, cursorY - 4.0f, fieldW, 18.0f };
			node.Enabled = true;
			node.Interactive = false;
			node.Visible = true;
			Wui::WuiAccessibility::Get().Register(node);
		}

		// ---- 底部按钮 ----
		const float footerY = frame.Y + frame.H - Wui::ModalFooterPadding - Wui::ModalFooterHeight;
		const float okW = 96.0f;
		const float cancelW = 96.0f;
		const float buttonsRight = frame.X + frame.W - 16.0f;
		const Wui::WuiRect okRect { buttonsRight - okW, footerY, okW, Wui::ModalFooterHeight };
		const Wui::WuiRect cancelRect { okRect.X - 8.0f - cancelW, footerY, cancelW, Wui::ModalFooterHeight };
		const bool canCreate = nameError.empty();
		const std::string okTooltip = canCreate
			? Wui::Tr("panel.content_browser.new_shader.ok.tooltip",
				"Write the starting code to the path shown above and open it in the material editor.")
			: nameError;
		const bool okClicked = ModalActionButton(ctx, Wui::HashId("shader.new.ok"), okRect,
			Wui::Tr("panel.content_browser.new_shader.ok", "Create"),
			okTooltip, canCreate, true, theme);
		const bool cancelClicked = ModalActionButton(ctx, Wui::HashId("shader.new.cancel"), cancelRect,
			Wui::Tr("panel.content_browser.new_shader.cancel", "Cancel"),
			Wui::Tr("panel.content_browser.new_shader.cancel.tooltip",
				"Close without writing anything (Esc)"),
			true, false, theme);
		bool closeRequested = false;
		if ((okClicked || (nameSubmitted && !justOpened)) && canCreate)
		{
			// 目录不存在就建(与材质向导同口径:确认前只提示,确认时才落盘)。
			std::error_code dirError;
			const std::filesystem::path absolute = m_Model.Root / std::filesystem::path(target);
			const std::filesystem::path parent = absolute.parent_path();
			if (!parent.empty() && !std::filesystem::is_directory(parent, dirError))
				std::filesystem::create_directories(parent, dirError);
			const std::string source = ShaderTemplateSource(templateIndex);
			std::string error;
			bool wrote = false;
			if (dirError)
			{
				error = "could not create folder: " + parent.generic_string();
			}
			else
			{
				// 临时文件 + 原子替换(与脚本编辑器/材质保存同一套写法);失败不留半成品。
				const std::filesystem::path temporary =
					absolute.parent_path() / (absolute.filename().string() + ".tmp-write");
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				if (!out.is_open())
					error = "could not write " + temporary.generic_string();
				else
				{
					out << source;
					out.close();
					std::error_code renameError;
					std::filesystem::rename(temporary, absolute, renameError);
					if (renameError)
					{
						// 目标已存在时 rename 在 Windows 上会失败:先删目标再改名(覆盖 = 用户已看到警告)。
						std::error_code removeError;
						std::filesystem::remove(absolute, removeError);
						renameError.clear();
						std::filesystem::rename(temporary, absolute, renameError);
					}
					if (renameError)
					{
						error = renameError.message();
						std::error_code cleanupError;
						std::filesystem::remove(temporary, cleanupError);
					}
					else
					{
						wrote = true;
					}
				}
			}
			if (!wrote)
			{
				m_NewShaderFailure = error.empty() ? std::string("could not write the .slang") : error;
				m_NewShaderFailureFor = target;
				NotifyAssetFailure(m_NewShaderFailure);
				WLD_CORE_WARN("New material shader failed: {0}", m_NewShaderFailure);
			}
			else
			{
				WLD_CORE_INFO("[material-ui] created shader {0} (template {1})", target, templateIndex);
				SelectCreated(m_Model.Root / std::filesystem::path(target), "new-shader");
				// 打开 = 同一个材质编辑器的**代码形态**(面板按扩展名决定布局)。
				m_Host.OpenMaterialEditor(target);
				m_Host.Notify(Wui::Tr("panel.content_browser.new_shader.created", "Created ") + target);
				closeRequested = true;
			}
		}
		else if (cancelClicked)
			closeRequested = true;
		else if (escapePressed)
		{
			// Esc 分层:先关展开的下拉,第二次才关模态。
			if (templatePopupWasOpen)
				ctx.ClosePopup(templateId);
			else if (folderPopupWasOpen)
				ctx.ClosePopup(folderId);
			else
				closeRequested = true;
		}
		Wui::EndModalFrame(ctx);
		if (closeRequested && ctx.Modal() == modalId)
			CloseNewShaderModal(ctx);
	}


void ContentBrowserPanel::RegisterSliceNode(const BrowserSlice& slice, const Wui::WuiRect& rect){
		std::error_code relativeError;
		const std::filesystem::path relative =
			std::filesystem::relative(slice.Path, m_Model.Root, relativeError);
		const std::string logical = relativeError ? slice.Path.filename().generic_string()
			: relative.generic_string();
		Wui::WuiAccessNode node;
		node.Id = Wui::HashId(("browser.slice." + logical).c_str());
		node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
		node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
		node.Kind = "asset-slice";
		node.Label = slice.Name;
		node.Value = logical;
		// M4-S2:类型名进悬停说明(读屏/脚本看不到徽标与配色,必须能读出"这是哪一类资产"——
		// 着色器与 `.wmat` 的区别正是这里)。类型文案本身已本地化,这里只负责拼装。
		std::string tooltip = slice.IsDir
			? Wui::Tr("panel.content_browser.slice.folder.tooltip", "Folder under the content root: ") + logical
			: Wui::Tr("panel.content_browser.slice.asset.tooltip", "Asset: ") + logical + " · "
				+ slice.TypeLabel
				+ Wui::Tr("panel.content_browser.slice.asset.hint",
					" — drag it onto a texture slot / material title");
		node.Tooltip = tooltip;
		node.Rect = rect;
		node.Enabled = true;
		node.Interactive = true;
		node.Visible = true;
		Wui::WuiAccessibility::Get().Register(node);
	}

}
