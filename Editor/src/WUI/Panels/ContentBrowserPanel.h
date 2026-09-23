#pragma once

#include "EditorPanel.h"
#include "EditorAssetTypes.h"
#include "World/Core/Asset/AssetTypeRegistry.h"
#include "World/Renderer/Material.h"
#include "World/Renderer/Texture.h"
#include "World/WUI/WuiWidget.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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
		// P4-UX15:重命名时被"藏起来"的后缀(提交时若用户没写后缀就补回)。
		std::string RenameExtension;
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

	// P4-UX14:内容区一个"切片"(网格格子 / 列表行)的展示数据。
	// 排序与绘制共用同一份,避免重复做类型推断与 stat;大小按需填充(网格只统计可见切片)。
	struct BrowserSlice
	{
		std::filesystem::path Path;
		bool IsDir = false;
		uintmax_t Size = 0;    // 仅 SizeKnown = true 时有效
		bool SizeKnown = false;
		std::string Name;      // 文件名(不含目录)
		std::string Type;      // EditorAssetTypes 的类型名(Folder / Scene / Material / …)
		// M4-S2/Slang-B1:类型枚举(切片绘制按它区分渲染:着色器的图标染色与类型徽标;
		// `.hlsl` 是 legacy,徽标/类型名带 (legacy) 提示)。
		EditorAssetKind Kind = EditorAssetKind::Unknown;
		// P4-U10:本地化后的类型文案(切片展示用);glTF/GLB 明确标成"导入源"。
		std::string TypeLabel;
		std::string Extension; // 小写扩展名(含点);文件夹为空。着色器:.slang(主)/ .hlsl(legacy)
	};

	class ContentBrowserPanel final : public EditorPanel
	{
	public:
		explicit ContentBrowserPanel(PanelHost& host);
		~ContentBrowserPanel();
		const char* Id() const override { return "content_browser"; }
		const char* Title() const override { return "Content Browser"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;
		// P4-UX16:面板级快捷键(三层路由的第 2 层)——内容浏览器有焦点时,
		// Ctrl+N / Ctrl+Shift+N 在本层被消费,不会落到引擎全局的 Ctrl+N(File ▸ New Scene)。
		bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt) override;
		// D10-10(用户 2026-09-19):导入位置选择器已搬到 EditorShell 的窗口级模态
		// (居中 + 全窗口挡输入),面板不再画自己的覆盖层;只保留 shell 需要的两个入口:
		// 导入成功后刷新列表(与工具栏 Refresh 同一条路径)。
		void RefreshContents();
		// 当前浏览目录:shell 打开导入模态时用它作默认落点(与双击导入/拖放同一约定)。
		const std::filesystem::path& CurrentDirectory() const { return m_Model.Current; }
		// P4-U13d:选中刚创建/刚生成的资产(逻辑路径相对内容根);目标不在内容根下或不存在 → false。
		// 与"新建材质/场景/脚本"同一条选中通道(SelectCreated),但会先把列表导航到目标所在目录,
		// 让用户/脚本立刻看到它 —— 创建预制体成功后由宿主调用。
		bool SelectAsset(const std::string& logicalPath, const char* op);

		// ---- U25-M2:E 写材质的工作流 ----
		// "新建材质"向导(Window ▸ New Material… 与内容浏览器 New ▸ Material… 共用这一条):
		// 模板下拉(Standard / Unlit-ish / Transparent / Additive)+ 名称 + 目录 + 实时落点回显,
		// 确认后写出 .wmat 并在材质编辑器里打开它。
		bool OpenNewMaterialWizard(std::string* message = nullptr);
		// Extract from Selection:以选中实体 MeshRenderer 的当前材质为初值走同一个向导
		// (默认名 <实体名>_material),创建后把新材质**赋回该实体**并打开编辑器。
		bool OpenNewMaterialFromSelection(std::string* message = nullptr);
		// ---- M4-S2/Slang-B1:Material Shader(`.slang`)新建向导 ----
		// 与"新建材质"同一套 U13d 交互(名称 + 目录 + 实时落点 + 覆盖警告),内容更简单:
		// 模板 = 起始代码(引擎默认表面函数 / 默认表面函数 + 注解参数示例),两条模板都带
		// 契约注释头(严格类型 / 组合采样器 / 显式 binding + 文档与迁移脚本指针)。
		// 入口 = `New ▶ Material Shader…`(资产类型注册表)+ 本函数(宿主/菜单可直接调)。
		// 产物固定 `.slang`;`.hlsl` 只作为 legacy 被读取。
		bool OpenNewShaderWizard(std::string* message = nullptr);

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
		// ---- P4-UX16:"新建资产"注册表 ----
		// 默认四类(Folder / Material / Scene / Script)只注册一次;菜单/右键菜单/快捷键
		// 全部读同一张表 —— 以后加类型不再改这里的 UI 代码。
		void RegisterDefaultAssetTypes();
		void UnregisterDefaultAssetTypes();
		// 按类型 id 在当前目录创建(菜单与快捷键共用的唯一入口);失败写 error。
		bool CreateAssetFromRegistry(const std::string& typeId, std::string* error);
		// 各类型的真正实现(注册表回调指向它们;dir = 目标目录,必须已存在)。
		bool CreateMaterialAsset(const std::filesystem::path& dir, std::string* error,
			std::filesystem::path* outPath);
		bool CreateSceneAsset(const std::filesystem::path& dir, std::string* error,
			std::filesystem::path* outPath);
		bool CreateScriptAsset(const std::filesystem::path& dir, std::string* error,
			std::filesystem::path* outPath);
		// "New ▶" 子菜单:画行 / 画展开的类型列表(工具条 `…` 与内容区空白右键共用)。
		// owner = 1(工具条菜单)/ 2(空白右键菜单);0 = 未展开。同一时刻只可能开一个菜单。
		bool RenderNewAssetRow(Wui::WuiContext& ctx, Wui::WuiId rowId, const Wui::WuiRect& row,
			const Wui::WuiTheme& theme);
		// idPrefix 决定子菜单项的无障碍 id(如 "browser.toolbar.menu.new." / "browser.blank.new.")。
		Wui::WuiRect RenderNewAssetItems(Wui::WuiContext& ctx, const char* idPrefix,
			const Wui::WuiRect& parentMenu, const Wui::WuiRect& clampArea, const Wui::WuiTheme& theme);
		// 失败提示统一入口(面板不自己弹模态)。
		void NotifyAssetFailure(const std::string& error);
		// ---- U25-M2 ---- 
		// 打开/关闭/绘制"新建材质"向导(居中模态,复用 U13d 的 Create Prefab 那套)。
		void CloseNewMaterialModal(Wui::WuiContext& ctx);
		void DrawNewMaterialModal(Wui::WuiContext& ctx);
		std::string NewMaterialBaseName() const;
		std::string NewMaterialTarget() const;
		std::string NewMaterialNameError() const;
		// ---- M4-S2/Slang-B1:`.slang` 新建向导(与材质向导同一套模态骨架)----
		void OpenNewShaderModal(Wui::WuiContext& ctx);
		void CloseNewShaderModal(Wui::WuiContext& ctx);
		void DrawNewShaderModal(Wui::WuiContext& ctx);
		std::string NewShaderTarget() const;
		std::string NewShaderNameError() const;
		// Slang-B1:新建文件的契约注释头(三条硬规则 + docs/dev/shader-contract.md 与迁移脚本指针)。
		static std::string ShaderContractHeader();
		// 起始代码:0 = 引擎默认表面函数(经 M4-S1 的 MaterialSurfaceCompiler 取,不抄一份);
		// 1 = 同一份 + 一段注解参数示例(教用户怎么写 `//! param …`)。
		static std::string ShaderTemplateSource(int templateIndex);
		// 模板 → MaterialDesc(只映射现有字段;真正的着色模型是 M3 的事)。
		static MaterialDesc MaterialDescForTemplate(int templateIndex, const std::string& name,
			const MaterialDesc& seed);
		// 跨窗口拖放:释放那一帧把 "file:<逻辑路径>" 交给编辑器侧的落点登记
		// (内容浏览器在主窗口、材质编辑器在另一个窗口 —— 见 Editor::AssetDropBridge)。
		void DeliverCrossWindowDrop(Wui::WuiContext& ctx);
		// 本面板窗口的客户区原点(屏幕物理像素)+ 全局光标 → 屏幕坐标。
		bool GlobalCursorScreen(const Wui::WuiContext& ctx, float* outX, float* outY) const;
		// 内容区切片进无障碍树:脚本/AI 通道按稳定 id 拿到某个文件切片的矩形
		// (跨窗口拖放测试要按住一个 .png 切片,没有节点就只能猜坐标)。
		void RegisterSliceNode(const BrowserSlice& slice, const Wui::WuiRect& rect);
		// 选中刚创建的资产(各类型共用;顺带让"新建后立刻改名/打开"有统一落点)。
		void SelectCreated(const std::filesystem::path& path, const char* op);
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
		// P4-UX14:内容区统一切片的两个绘制入口(网格 / 列表 + 常驻表头)。
		// interact = 面板既有的选中/双击/拖拽/右键处理,按切片路径调用。
		void RenderGridSlices(Wui::WuiContext& ctx, const Wui::WuiRect& area, const Wui::WuiTheme& theme,
			const std::vector<BrowserSlice>& slices,
			const std::function<void(const std::filesystem::path&, const Wui::WuiRect&, bool)>& interact,
			bool treeRenameDrawn);
		void RenderListSlices(Wui::WuiContext& ctx, const Wui::WuiRect& area, const Wui::WuiTheme& theme,
			const std::vector<BrowserSlice>& slices, int& sortColumn, bool& sortAscending,
			const std::function<void(const std::filesystem::path&, const Wui::WuiRect&, bool)>& interact,
			bool treeRenameDrawn);

		PanelHost& m_Host;
		ContentBrowserModel m_Model;
		// ---- U25-M2:"新建材质"向导(面板级模态)----
		bool m_NewMaterialOpen = false;
		uint32_t m_NewMaterialOpenedFrame = 0;
		std::string m_NewMaterialName;
		std::vector<std::string> m_NewMaterialFolders;
		int m_NewMaterialFolderIndex = 0;
		int m_NewMaterialTemplate = 0;        // 0=Standard 1=Unlit-ish 2=Transparent 3=Additive
		// M3:Parent 下拉 —— 下标 0 = 引擎内置默认(自包含材质),> 0 = 已有 .wmat(继承它,
		// 文件里只写覆盖字段)。路径表与下拉一一对应。
		std::vector<std::string> m_NewMaterialParentPaths;
		int m_NewMaterialParentIndex = 0;
		std::string m_NewMaterialFailure;
		std::string m_NewMaterialFailureFor;
		bool m_NewMaterialFromSelection = false;
		MaterialDesc m_NewMaterialSeed;       // Extract 的初值(选中实体当前材质 / 它的 Color)
		Entity m_NewMaterialAssignEntity;     // Extract:创建后要赋回哪个实体
		// 向导可能在"非渲染期"被请求(注册表回调 / Window 菜单 / 材质面板的 Extract):
		// 统一排队,下一帧的 OnRender 里打开(那时才有 WuiContext 与布局)。
		bool m_NewMaterialPendingOpen = false;
		bool m_NewMaterialPendingFromSelection = false;
		std::filesystem::path m_NewMaterialPendingDir;
		void OpenNewMaterialModal(Wui::WuiContext& ctx, bool fromSelection);
		// ---- M4-S2/Slang-B1:`.slang` 新建向导状态 ----
		bool m_NewShaderOpen = false;
		uint32_t m_NewShaderOpenedFrame = 0;
		std::string m_NewShaderName;
		std::vector<std::string> m_NewShaderFolders;
		int m_NewShaderFolderIndex = 0;
		int m_NewShaderTemplate = 0;   // 0 = 默认表面函数 / 1 = 默认 + 注解参数示例
		std::string m_NewShaderFailure;
		std::string m_NewShaderFailureFor;
		bool m_NewShaderPendingOpen = false;
		std::filesystem::path m_NewShaderPendingDir;
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
		// P4-UX14:工具条只剩「面包屑 + 搜索 + ⋯」;低频动作收进 ⋯ 菜单。
		std::shared_ptr<Wui::WuiButton> m_MoreButton;
		bool m_ToolbarMenuOpen = false;
		glm::vec2 m_ToolbarMenuPos { 0, 0 };
		// P4-UX16:"New ▶" 子菜单当前归属(0 = 未展开;1 = 工具条 `…` 菜单;2 = 内容区空白右键菜单)。
		int m_NewMenuOwner = 0;
		// OnShortcut 在渲染之外触发 → 只排队,真正的动作在下一帧的 OnRender 里做(那时有 ctx)。
		// 1 = 打开"新建"列表,2 = 直接新建文件夹。
		int m_PendingNewShortcut = 0;
		// 注册表登记状态:析构时必须反注册,否则回调会指向已销毁的面板。
		bool m_AssetTypesRegistered = false;
		std::vector<std::shared_ptr<Wui::WuiButton>> m_CrumbButtons;
		std::vector<std::filesystem::path> m_CrumbDests;
		// P4-UX15:面包屑改立即模式绘制,标签单独存一份(不再依赖嵌套 Box 的布局)。
		std::vector<std::string> m_CrumbLabels;
		std::filesystem::path m_LastCrumbPath;
		std::filesystem::path m_StatePath;
		Wui::WuiContext* m_Ctx = nullptr;
	};
}
