#include "ContentBrowserPanel_Internal.h"

namespace World
{

using namespace ContentBrowserPanelDetail;


	// ---- P4-UX16:"新建资产"注册表 ----
	// 用户 2026-09-20:「不能每加一个类似的资产就新增一个按钮」。
	// 这里把"有哪些资产类型、怎么创建"收敛成**一次注册**;菜单/右键菜单/快捷键只读注册表,
	// 以后加 Model / Texture 等类型 → 再 Register 一条,内容浏览器的 UI 代码不动。
	// 生命周期:Create 回调捕获 this,析构必须反注册(见 ~ContentBrowserPanel)。
void ContentBrowserPanel::RegisterDefaultAssetTypes(){
		if (m_AssetTypesRegistered)
			return;
		m_AssetTypesRegistered = true;

		AssetTypeRegistry& registry = AssetTypeRegistry::Get();
		auto add = [&registry](const char* id, const char* label, const char* extension, int order,
			bool isFolder, std::function<bool(const std::filesystem::path&, std::string*)> create)
		{
			AssetTypeDesc desc;
			desc.Id = id;
			desc.Label = label;
			desc.Extension = extension;
			desc.SortOrder = order;
			desc.IsFolder = isFolder;
			desc.Create = std::move(create);
			registry.Register(std::move(desc));
		};

		// Folder:唯一"建完立刻改名"的类型(与资源管理器同款手感,沿用 D10-6 的既有路径)。
		add("folder", "Folder", "", 0, true,
			[this](const std::filesystem::path& dir, std::string* error)
			{
				if (!m_Ctx)
				{
					if (error) *error = "content browser has no active UI context";
					return false;
				}
				if (CreateFolderIn(*m_Ctx, dir).empty())
				{
					if (error) *error = "could not create a folder under " + dir.string();
					return false;
				}
				return true;
			});
		// Material(U25-M2 起):走**新建向导** —— 模板 / 名称 / 目录 / 实时落点全部在确认前可见,
		// 创建成功后由向导自己选中新资产并在材质编辑器里打开它。这里只负责把向导排进下一帧
		// (注册表回调也可能从快捷键路径进来,那时没有 WuiContext)。
		add("material", "Material", ".wmat", 10, false,
			[this](const std::filesystem::path& dir, std::string* error)
			{
				(void)error;
				m_NewMaterialPendingDir = dir;
				m_NewMaterialPendingOpen = true;
				m_NewMaterialPendingFromSelection = false;
				return true;
			});
		// Slang-B1:Material Shader(`.slang`,主扩展名)—— 与材质同一套"先向导后创建":
		// 名称/目录/实时落点全部在确认前可见,确认后写出起始代码(含三条硬规则的注释头)
		// 并在材质编辑器的**代码形态**里打开。产物固定 `.slang`。
		add("shader", "Material Shader", ".slang", 15, false,
			[this](const std::filesystem::path& dir, std::string* error)
			{
				(void)error;
				m_NewShaderPendingDir = dir;
				m_NewShaderPendingOpen = true;
				return true;
			});
		// Scene:只创建 + 选中,**不自动打开** —— 打开会直接替换当前文档
		// (EditorLayer::DoOpenScene 不拦未保存改动),"新建资产"不该顺带丢掉用户正在编辑的场景。
		add("scene", "Scene", ".wd", 20, false,
			[this](const std::filesystem::path& dir, std::string* error)
			{
				std::filesystem::path created;
				return CreateSceneAsset(dir, error, &created);
			});
		// PECS-T11:Lua 只有**一个**入口 —— 旧 `Script`(落右键目录、`.lua`,在 assets/ 根
		// 新建就永远不会被加载)与 T9 的 `Lua System`(固定 scripts/systems/)合并成
		// 「Lua Script…」:点它打开宿主**同一个**「新建 Lua …」向导(系统 / 脚本库 / 空
		// 在向导里选),落点/模板/扩展名全由向导保证,面板不再自己写盘。
		// 面板渲染期拿不到安全的模态打开点 ⇒ 只置标记,宿主在帧边界取走(与 C++ 的
		// `RequestNewCppScript()` 同一件事,只是那条走 PanelHost 虚函数、这里走帧边界标记)。
		add("script", "Lua Script…", ".luau", 30, false,
			[this](const std::filesystem::path& dir, std::string* error)
			{
				(void)dir;     // 落点由向导按类型决定(scripts/systems|lib|/),与右键目录无关
				(void)error;   // 需要项目/内容根时由向导给可读提示,这里不拦
				m_PendingLuaWizard = true;
				return true;
			});
	}


void ContentBrowserPanel::UnregisterDefaultAssetTypes(){
		if (!m_AssetTypesRegistered)
			return;
		m_AssetTypesRegistered = false;
		AssetTypeRegistry& registry = AssetTypeRegistry::Get();
		// 只撤销本面板注册的 id:别的组件(宿主/插件/测试)注册的类型不受影响。
		for (const char* id : { "folder", "material", "shader", "scene", "script" })
			registry.Unregister(id);
	}


bool ContentBrowserPanel::CreateAssetFromRegistry(const std::string& typeId, std::string* error){
		const AssetTypeDesc* desc = AssetTypeRegistry::Get().Find(typeId);
		if (!desc || !desc->Create)
		{
			if (error) *error = "asset type is not registered: " + typeId;
			return false;
		}
		std::error_code dirError;
		if (!std::filesystem::is_directory(m_Model.Current, dirError))
		{
			if (error) *error = "target folder does not exist: " + m_Model.Current.string();
			return false;
		}
		std::string localError;
		if (!desc->Create(m_Model.Current, &localError))
		{
			if (error) *error = localError.empty() ? ("could not create " + typeId) : localError;
			return false;
		}
		return true;
	}


void ContentBrowserPanel::SelectCreated(const std::filesystem::path& path, const char* op){
		m_Model.Selected.clear();
		m_Model.Selected.insert(path);
		m_Model.LastSelected = path;
		InvalidateContents();
		SaveState();
		if (m_Ctx && op)
			m_Ctx->RecordOp("browser", op, path.filename().string(), "");
	}


	// P4-U13d:按逻辑路径选中一个已存在的资产(创建预制体成功后由宿主调用)。
	// 选中的路径存的是**绝对路径**(与 SelectCreated / 内容区切片一致);目标不在当前目录时
	// 先导航过去(导航会清选中,所以选中必须放在导航之后)。
bool ContentBrowserPanel::SelectAsset(const std::string& logicalPath, const char* op){
		std::string normalized = logicalPath;
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		if (normalized.empty())
			return false;
		const std::filesystem::path absolute = m_Model.Root / std::filesystem::path(normalized);
		std::error_code ec;
		if (!std::filesystem::exists(absolute, ec))
			return false;
		if (absolute.parent_path() != m_Model.Current)
			Navigate(absolute.parent_path());
		SelectCreated(absolute, op);
		return true;
	}


bool ContentBrowserPanel::CreateMaterialAsset(const std::filesystem::path& dir, std::string* error, std::filesystem::path* outPath){
		// 写一份默认 .wmat 模板(重名自动编号)。
		// 关键:MaterialIO 的路径解析只认"内容根/<path>"(World::Paths::AssetRoot()),绝对路径原样命中。
		// 历史:早先还会回退 `<Game>/<path>`,相对逻辑路径**新建**时会落到项目目录之外
		// (实测:内容浏览器新建材质写进了 Game/_ux_probe)。所以这里始终传绝对路径(旧 CreateMaterial 的同一个坑)。
		const std::filesystem::path target = MakeUniqueAssetPath(dir, "material", ".wmat");
		MaterialDesc desc;
		desc.Name = target.stem().string();
		std::string localError;
		if (!MaterialIO::WriteFileText(target.generic_string(), MaterialIO::Serialize(desc, GenerateAssetId()), &localError))
		{
			WLD_CORE_ERROR("Could not create material: {0}", localError);
			if (error) *error = localError;
			return false;
		}
		NotifyAssetWritten(target);
		SelectCreated(target, "new-material");
		if (outPath) *outPath = target;
		return true;
	}


bool ContentBrowserPanel::CreateSceneAsset(const std::filesystem::path& dir, std::string* error, std::filesystem::path* outPath){
		// 用引擎自己的 SceneSerializer 写"空场景",而不是手写模板字符串:
		// 产物与编辑器另存出来的场景同格式(FormatVersion/Entities),格式演进时不会两处漂移。
		const Ref<Scene> active = m_Host.GetActiveScene();
		if (!active)
		{
			if (error) *error = "no active scene to derive a world context from";
			return false;
		}
		const std::filesystem::path target = MakeUniqueAssetPath(dir, "scene", ".wd");
		SceneSerializer serializer(CreateRef<Scene>(active->GetContext()));
		if (!serializer.Serialize(target.string()))
		{
			const std::string localError = serializer.GetLastError().empty()
				? ("could not write " + target.string()) : serializer.GetLastError();
			WLD_CORE_ERROR("Could not create scene: {0}", localError);
			if (error) *error = localError;
			return false;
		}
		SelectCreated(target, "new-scene");
		if (outPath) *outPath = target;
		return true;
	}


	// PECS-T11:合并后的「Lua Script…」入口只排队 —— 宿主在帧边界取走标记并打开
	// 同一个「新建 Lua …」向导(见 EditorShell::OnRender 的帧边界分支)。
bool ContentBrowserPanel::ConsumePendingLuaWizardRequest(){
		if (!m_PendingLuaWizard)
			return false;
		m_PendingLuaWizard = false;
		return true;
	}


bool ContentBrowserPanel::RenderNewAssetRow(Wui::WuiContext& ctx, Wui::WuiId rowId, const Wui::WuiRect& row, const Wui::WuiTheme& theme){
		// 子菜单指示三角用 ▶(U+25B6):字体子集里验证过的字形(树用 ▼/▶);
		// "▸"(U+25B8)与"⋯"(U+22EF)一样不在子集里,会画成乱码。
		const std::string label = Wui::Tr("panel.content_browser.menu.new", "New") + "  ▶";
		return Wui::MenuItem(ctx, rowId, row, label, true, theme);
	}


Wui::WuiRect ContentBrowserPanel::RenderNewAssetItems(Wui::WuiContext& ctx, const char* idPrefix, const Wui::WuiRect& parentMenu, const Wui::WuiRect& clampArea, const Wui::WuiTheme& theme){
		// 清单 = 资产类型注册表(排序在注册表里定:Folder 恒第一 → SortOrder → Id)。
		// 这里**没有任何按类型分支** —— 新类型注册进来就自动出现在菜单里。
		const std::vector<AssetTypeDesc> types = AssetTypeRegistry::Get().Sorted();
		if (types.empty())
			return {};

		const float itemH = 22.0f;
		Wui::WuiRect panel { parentMenu.X + parentMenu.W - 4.0f, parentMenu.Y + 4.0f, 236.0f,
			itemH * static_cast<float>(types.size()) + 8.0f };
		// 贴边翻转:右侧放不下就摆到父菜单左侧;再夹进面板可视区(子菜单永远不出画面)。
		if (panel.X + panel.W > clampArea.X + clampArea.W - 4.0f)
			panel.X = parentMenu.X - panel.W + 4.0f;
		panel.X = std::max(clampArea.X + 4.0f,
			std::min(panel.X, clampArea.X + clampArea.W - panel.W - 4.0f));
		panel.Y = std::max(clampArea.Y + 4.0f,
			std::min(panel.Y, clampArea.Y + clampArea.H - panel.H - 4.0f));
		Wui::DrawPanelSurface(ctx, panel, theme);
		// P4-U7:子菜单往父菜单右侧伸出,父菜单矩形的遮挡区盖不住它 —— 子菜单矩形必须
		// 自己登记(下一帧树/切片不会吃掉落在子菜单上的点击)。
		ctx.RegisterOverlayRect(panel);

		const std::string tooltip = Wui::Tr("panel.content_browser.new.tooltip", "Create in the current folder");
		for (size_t index = 0; index < types.size(); ++index)
		{
			const AssetTypeDesc& desc = types[index];
			const Wui::WuiRect row { panel.X + 4.0f, panel.Y + 4.0f + itemH * static_cast<float>(index),
				panel.W - 8.0f, itemH };
			const std::string label = Wui::Tr(("asset.type." + desc.Id).c_str(), desc.Label.c_str());
			const Wui::WuiId itemId = Wui::HashId((std::string(idPrefix) + desc.Id).c_str());
			const bool clicked = Wui::MenuItem(ctx, itemId, row, label, true, theme);
			// MenuItem 登记的无障碍节点只有"文字标签";这里用同一个 id 再登记一次(后写覆盖),
			// 把**扩展名**与**创建位置**写进 Value/Tooltip —— 右侧那行扩展名提示是画出来的,
			// 读屏与脚本读不到,必须同时进节点(用户 2026-09-18:「引擎的 UI 对 AI 要无障碍」)。
			{
				Wui::WuiAccessNode node;
				node.Id = itemId;
				node.Window = Wui::WuiAccessibility::Get().CurrentWindow();
				node.Panel = Wui::WuiAccessibility::Get().CurrentPanel();
				node.Kind = "menu-item";
				node.Label = label;
				// 文件夹没有扩展名 → 值写明 "(folder)",脚本据此区分"目录"与"文件类型"。
				node.Value = desc.Extension.empty() ? "(folder)" : desc.Extension;
				node.Tooltip = tooltip;
				node.Rect = row;
				Wui::WuiAccessibility::Get().Register(node);
			}
			if (clicked)
			{
				std::string error;
				if (!CreateAssetFromRegistry(desc.Id, &error))
					NotifyAssetFailure(error);
				m_NewMenuOwner = 0;
				ctx.CloseAllPopups();
				break;
			}
			// 右侧扩展名提示(材料 → .wmat / 场景 → .wd / 脚本 → .lua),一眼可辨且与"另存为"同名。
			if (!desc.Extension.empty())
			{
				const float textWidth = ctx.MeasureTextWidth(desc.Extension, 12.0f);
				Wui::Label(ctx, { row.X + row.W - textWidth - 8.0f, row.Y + (row.H - 15.0f) * 0.5f },
					desc.Extension, theme.TextMuted, 12.0f);
			}
			Wui::Tooltip(ctx, row, tooltip);
		}
		return panel;
	}


void ContentBrowserPanel::NotifyAssetFailure(const std::string& error){
		const std::string text = Wui::Tr("panel.content_browser.new.failed", "Could not create asset: ") + error;
		WLD_CORE_ERROR("[content-browser] {0}", text);
		m_Host.Notify(text);
	}


bool ContentBrowserPanel::OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt){
		(void)alt;
		// P4-UX16:Ctrl+N = 打开"新建"清单;Ctrl+Shift+N = 新建文件夹(资源管理器同款)。
		// 本函数是三层路由的第 2 层:内容浏览器**有焦点**时优先于引擎全局 Ctrl+N
		// (File ▸ New Scene);没焦点时那条全局命令照旧生效,不抢别处。
		if (!ctrl || keyCode != KeyCodes::N)
			return false;
		// 快捷键在渲染之外到达(拿不到 ctx),只排队;真正的动作在下一帧 OnRender 里做。
		m_PendingNewShortcut = shift ? 2 : 1;
		return true;
	}


void ContentBrowserPanel::ApplyRename(const std::filesystem::path& target, const std::string& newName){
		if (newName.empty())
		{
			m_Model.RenameTarget.clear();
			m_Model.RenameActive = false;
			m_TreeRenameTarget.clear();
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
		m_TreeRenameTarget.clear();
		InvalidateContents();
		SaveState();
		if (m_Model.Search[0])
			UpdateSearch();
	}


void ContentBrowserPanel::StartRename(Wui::WuiContext& ctx, const std::filesystem::path& path){
		m_Model.RenameTarget = path;
		// P4-UX15:重命名默认只编辑**主名**,后缀单独保留 —— 与资源管理器一致
		// (用户:"重命名时还是会显示后缀名")。提交时若用户没写后缀,自动补回原名后缀。
		m_Model.RenameExtension = path.extension().string();
		m_Model.RenameEdit = m_Model.RenameExtension.empty() ? path.filename().string() : path.stem().string();
		std::strncpy(m_Model.RenameBuffer, m_Model.RenameEdit.c_str(), sizeof(m_Model.RenameBuffer) - 1);
		m_Model.RenameActive = true;
		ctx.SetFocus(Wui::HashId("browser.rename"));
		ctx.SetTextInputActive(true);
	}


void ContentBrowserPanel::RenderRenameField(Wui::WuiContext& ctx, const std::filesystem::path& path, const Wui::WuiRect& rect, const Wui::WuiTheme& theme){
		const Wui::WuiId renameId = Wui::HashId("browser.rename");
		// U2d:输入非法(空 / 非法字符 / 同目录重名)时用 TextFieldEx 行内显示原因并拒绝提交。
		// TextFieldEx 没有 cancelled 回调,所以 Esc 取消在这里先于控件处理(否则会把"取消"
		// 当成失焦提交,把非法名字写进 ApplyRename —— 语义就变了)。
		const bool cancelRequested = ctx.Focus() == renameId && ctx.IsKeyPressed(KeyCodes::Escape);
		const std::string error = RenameErrorFor(path, m_Model.RenameEdit);
		const bool submitted = Wui::TextFieldEx(ctx, renameId, rect, m_Model.RenameEdit, theme, error);
		if (cancelRequested)
		{
			// Escape 丢弃(与旧 TextField 的 cancelled 语义相同)。
			m_Model.RenameTarget.clear();
			m_Model.RenameEdit.clear();
			m_Model.RenameActive = false;
			m_TreeRenameTarget.clear();
		}
		else if (submitted)
		{
			// 回车提交:非法名字拒绝提交。TextFieldCore 在提交帧会自己失焦,这里把焦点还给
			// 输入框 —— 否则下一帧就按"失焦提交"把框收掉,错误只闪一帧、也没法继续改。
			if (error.empty())
				ApplyRename(path, RenameNameWithExtension(path, m_Model.RenameEdit));
			else
			{
				WLD_CORE_WARN("[browser] rename rejected: {0}", error);
				ctx.SetFocus(renameId);
				ctx.SetTextInputActive(true);
			}
		}
		else if (m_Model.RenameActive && ctx.Focus() != renameId)
		{
			// 失焦提交:点选其他条目或空白处时收起重命名框。非法名字不提交(原名字不变,
			// 也不留一个失去焦点、无法继续编辑的输入框)。
			if (error.empty())
				ApplyRename(path, RenameNameWithExtension(path, m_Model.RenameEdit));
			else
			{
				WLD_CORE_WARN("[browser] rename rejected on blur: {0}", error);
				m_Model.RenameTarget.clear();
				m_Model.RenameEdit.clear();
				m_Model.RenameActive = false;
				m_TreeRenameTarget.clear();
			}
		}
	}


void ContentBrowserPanel::OpenInExplorer(const std::filesystem::path& path){
		// /select 让资源管理器打开所在文件夹并选中该项,而不是直接打开文件。
		const std::wstring parameters = L"/select,\"" + std::filesystem::absolute(path).wstring() + L"\"";
		const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
		if (reinterpret_cast<intptr_t>(result) <= 32)
			WLD_CORE_WARN("Could not open Explorer for '{0}' (error {1})", path.string(), reinterpret_cast<intptr_t>(result));
	}


void ContentBrowserPanel::OpenFolderInExplorer(const std::filesystem::path& path){
		// D10-6:直接打开该目录本身(面板既有的系统调用风格,见 OpenItem 的兜底分支)。
		const std::string cmd = "start \"\" explorer.exe \"" + std::filesystem::absolute(path).string() + "\"";
		system(cmd.c_str());
	}


void ContentBrowserPanel::Cut(){
		m_Model.Clipboard.assign(m_Model.Selected.begin(), m_Model.Selected.end());
		m_Model.ClipboardCut = true;
	}


void ContentBrowserPanel::Copy(){
		m_Model.Clipboard.assign(m_Model.Selected.begin(), m_Model.Selected.end());
		m_Model.ClipboardCut = false;
	}


void ContentBrowserPanel::SelectAll(const std::vector<std::filesystem::path>& paths){
		m_Model.Selected.clear();
		m_Model.Selected.insert(paths.begin(), paths.end());
		if (!paths.empty())
			m_Model.LastSelected = paths.back();
	}


	// D10-10(用户 2026-09-19):导入位置选择器已搬到 EditorShell 的窗口级模态(居中 + 全窗口挡输入)。
	// 面板这里只保留 shell 需要的刷新入口(与工具栏 Refresh / 内容区菜单同一条路径)。
void ContentBrowserPanel::RefreshContents(){
		InvalidateContents();
		if (m_Model.Search[0])
			UpdateSearch();
	}

}
