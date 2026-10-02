#pragma once

#include "Panels/ContentBrowserPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/InputMapPanel.h"
#include "Panels/PropertiesPanel.h"
#include "Panels/ReadoutPanels.h"
#include "Panels/SavePanel.h"
#include "Panels/LevelPanel.h"
#include "Panels/MaterialEditorPanel.h"
#include "Panels/TextureSettingsPanel.h"
#include "Panels/ScriptEditorPanel.h"
#include "Panels/ScriptsPanel.h"
#include "Panels/ViewportPanel.h"
#include "Panels/WidgetGalleryPanel.h"
#include "Panels/WindowsPanel.h"
#include "Panels/AttachSlotPanel.h"
#include "Panels/PluginsPanel.h"
#include "Panels/PluginPanel.h"
#include "PluginEditorHost.h"
#include "FloatWindowHost.h"
#include "../Project/ProjectLauncher.h"
#include "../Project/ProjectScaffolder.h"
#include "../Project/PluginScaffolder.h"

#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiDock.h"
#include "World/WUI/WuiWidgets.h"

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	class EditorLayer;
	class PrefabPanel;
	namespace Plugins { class PluginManager; }

	// 编辑器外壳:停靠布局、菜单栏、模态与面板注册表。业务面板已组件化到
	// Panels/ 目录,外壳只负责驱动它们渲染并通过 PanelHost / ViewportHost 供给能力。
	class EditorShell : public ViewportHost
	{
	public:
		// PROJ-3/T1:launcherMode = 纯启动器模式 —— 窗口只画一页"项目启动器":
		// 不读/不写停靠布局存档(local/wui-layout.json),不恢复上次的独立窗口,
		// 也不画菜单栏 / 挂靠栏 / 停靠区 / 状态栏(渲染分支见 OnRender)。
		explicit EditorShell(EditorLayer& editor, bool launcherMode = false);
		~EditorShell();
		void OnRender(Wui::WuiContext& ctx);
		// 退出前释放独立窗口(释放其呈现目标/OS 窗口,必须在 RHI 设备销毁前调用)。
		void ReleaseIndependentWindows();
		// ---- AI 控制通道(与 Window 菜单 / 内容浏览器同一条开关路径)----
		// 打开/关闭面板(独立窗口复用已隐藏窗口;材质面板按需创建实例)。
		bool AiTogglePanel(const std::string& panel);
		// P3-1②:脚本化的"分离 / 挂回"(只对声明为独立窗口形态的面板有意义)。
		// 内部复用既有 OpenIndependentPanel / AttachIndependentWindowToSlot 一对路径:
		//   分离 = 独立 OS 窗口显示(附加态先摘掉顶栏标签);挂回 = OS 窗口隐藏 + 顶栏标签。
		// 已处于目标状态时幂等,可读结果写进 message(供脚本直接断言)。
		bool AiDetachPanel(const std::string& panel, std::string* message);
		bool AiAttachPanel(const std::string& panel, std::string* message);
		// 抓一张独立窗口的合成画面(下一帧写盘)。
		bool AiRequestFloatCapture(const std::string& panel, const std::string& path);
		// 抓一张材质面板的预览纹理(下一帧写盘;RHI 读回,双后端有效)。
		bool AiRequestPreviewCapture(const std::string& panel, const std::string& path);
		// 调整独立窗口尺寸(等价于用户拖动窗口边框)。
		bool AiResizeWindow(const std::string& panel, float width, float height);
		// 面板/窗口/材质面板状态(JSON 文本,供 state.dump)。
		std::string AiDescribeState() const;
		// 当前停靠布局(JSON 文本,供 ui.layout.get)。
		std::string AiLayoutJson() const { return m_Layout.Serialize(); }
		// U26:窗口几何(物理像素客户区原点 + 尺寸),按**无障碍 window 键**寻址 ——
		// 跨窗口脚本要把 a11y 矩形(设计单位)换算成屏幕物理像素,必须先知道目标窗口
		// 客户区的屏幕原点;让脚本自己 EnumWindows 猜窗口角色(主窗口/浮窗/GLFW 消息窗/
		// 驱动的 pbuffer 窗口)在实测里错过一次,这里由引擎直接给出权威答案(供 ui.windows)。
		std::string AiWindowRectsJson() const;
		// 渲染后端切换后重建全部可见独立窗口(位置/尺寸/面板归属保留)。
		void RecreateIndependentWindows();
		Wui::WuiRect ViewportRect() const { return m_ViewportRect; }

		// ---- PanelHost ----
		Ref<Scene> GetActiveScene() override;
		Entity GetSelectedEntity() override;
		void SetSelectedEntity(Entity entity) override;
		void MarkDocumentDirty() override;
		void DuplicateSelectedEntity() override;
		void OpenScene(const std::filesystem::path& path) override;
		Wui::WuiTheme& Theme() override { return m_Theme; }

		// ---- ViewportHost ----
		bool HasRenderedScene() const override;
		Ref<SceneRenderer>& GetSceneRenderer() override;
		void SetViewportState(bool focused, bool hovered, glm::vec2 size, glm::vec2 bounds[2]) override;
		void SetViewportRect(const Wui::WuiRect& rect) override;
		bool IsPlaying() const override;
		bool IsSimulating() const override;
		bool IsPaused() const override;
		void TogglePlay() override;
		void ToggleSimulate() override;
		void TogglePause() override;
		Ref<Texture2D> GetIcon(int index) const override;
		uint64_t GetIconId(int index) const override;
		uint32_t TextureEpoch() const override;
		uint64_t GetSceneTextureId() const override;
		// 相机可视化:预览小窗(PiP)的纹理/开关/标签。
		uint64_t GetCameraPreviewTextureId() const override;
		bool IsCameraPreviewEnabled() const override;
		void ToggleCameraPreview() override;
		std::string CameraPreviewLabel() const override;
		// ---- 独立窗口(与停靠面板不同的组件)----
		// 计数/索引均按"窗口"而不是"面板":一个窗口可承载多个面板(标签栏)。
		size_t IndependentWindowCount() const override;
		std::string IndependentWindowPanel(size_t index) const override;
		std::string IndependentWindowLabel(size_t index) const override;
		void FocusIndependentWindow(const std::string& panel) override;
		void DockBackIndependentWindow(const std::string& panel) override;
		// D3:打开材质编辑器(必要时先打开独立窗口)并载入指定材质。
		void OpenMaterialEditor(const std::string& path) override;
		// 动态材质面板(每个材质一个 "material:<path>" 面板):从布局存档恢复时按 id 建实例。
		void EnsureMaterialPanelFromId(const std::string& panelId);
		// P1b D5:动态模型预览面板("model:<逻辑路径>")从布局存档恢复时按 id 建实例。
		void EnsureModelPanelFromId(const std::string& panelId);
		// P4-U13c:动态 prefab 资产窗口("prefab:<逻辑路径>")从布局存档/AI ui.open 恢复时按 id 建实例。
		void EnsurePrefabPanelFromId(const std::string& panelId);
		// W9-2:打开脚本编辑器(每个脚本一个 "script:<逻辑路径>" 面板,创建后默认附加到主窗口)。
		void OpenScriptEditor(const std::string& logicalPath) override;
		// 帧边界执行版:内部使用(AI 通道在帧首、OnRender 开头处理待办时)。
		void OpenScriptEditorNow(const std::string& logicalPath);
		// CPPSRC-1:项目 C++ 的唯一展示面 = 内容浏览器的"项目 C++"根
		// (EditorPanel.h 的 PanelHost 契约)。把面板拉回停靠树 → 激活 → 切根;
		// 失败(false)= 没当前项目 / 没有 `<项目根>/src` / 面板不可用。
		bool FocusContentBrowserProjectSources() override;
		// CPPSRC-1:内容浏览器里的"新建 C++ 组件…"(与 File ▸ 新建 C++ 组件… 同一个向导)。
		bool RequestNewCppScript() override;
		// ---- PLUG-T3:插件管理器面板的数据与动作 ----
		// 面板只依赖 shell(与其它面板同一条纪律):数据源 = EditorLayer 的 PluginManager。
		// 插件系统未接线(启动器形态 / 未打开项目 / 未初始化)⇒ nullptr。
		// E2:插件管理器只在项目形态可用(启动器形态不注册面板、菜单不出现)。
		Plugins::PluginManager* GetPluginManager() const;
		bool PluginManagerAvailable() const;
		// 本机禁用清单(`local/plugins.json`)当前是否包含该引擎插件。
		bool IsPluginDisabled(const std::string& id) const;
		// 本次启动后禁用状态被改过(启动时那份清单 ≠ 现在的清单)⇒ 面板给"需重启"提示。
		bool PluginRestartPending(const std::string& id) const;
		// 本次启动加载该插件失败的可读原因(空 = 没有失败记录;面板/`plugin.info` 与拒绝诊断并列)。
		std::string PluginLoadError(const std::string& id) const;
		// 启用/禁用**引擎插件**(写 `local/plugins.json`,下次启动生效)。
		// 项目插件(随项目加载)返回 false + 可读理由;找不到 id 同样 false + 理由。
		bool SetPluginEnabled(const std::string& id, bool enabled, std::string* message = nullptr);
		// ---- PLUG-T3b:插件贡献的编辑器命令 / 面板 ----
		// 编辑器侧的插件扩展宿主(命令注册表 + 面板注册表;由 EditorLayer 在插件加载前
		// 通过 PluginManager::SetEditorHost 注入)。未接线 = nullptr。
		PluginEditorHost* GetPluginEditorHost() const { return m_PluginEditorHost.get(); }
		// 插件面板注册表 id 前缀(`plugin.panel.<pluginId>.<id>`)。
		static bool IsPluginPanelId(const std::string& panelId);
		// 面板注册表里的插件面板补建实例(插件加载全部结束后由 EditorLayer 调一次;
		// 布局恢复 / AI ui.open 也走同一条 ensure 路径)。
		void EnsurePluginPanelsFromRegistry();
		void EnsurePluginPanelFromId(const std::string& panelId);
		// 插件卸载后关闭对应面板(注册表里已经没有了;防止空窗口留在布局里)。
		void ClosePluginPanelsNotInRegistry();
		// 插件面板的渲染(PluginPanel 容器转发到这里;内部走 PluginEditorHost → PluginManager
		// → 插件的 Draw)。面板已注销 = 画一行可读空态,不静默画旧内容。
		void DrawPluginPanel(Wui::WuiContext& ctx, const Wui::WuiRect& rect, const std::string& panelId);
		// 触发一条插件编辑器命令(名字 = plugin.command.<pluginId>.<id>)。
		bool InvokePluginEditorCommand(const std::string& commandName, std::string* message = nullptr);
		// 启动器形态(PROJ-3/T1):插件管理器与插件扩展面在启动器形态整体缺席。
		bool IsLauncherMode() const { return m_LauncherMode; }
		// 插件面板 Draw 桥在**帧内**取当前绘制环境(由 PluginEditorHost 调用;
		// 只在 RenderPanelContent 正在渲染插件面板时有效)。
		bool HasWuiContext() const { return m_Ctx != nullptr && m_CurrentPluginPanelRect.W > 0.0f; }
		Wui::WuiContext& WuiContextRef() const { return *m_Ctx; }
		const Wui::WuiRect& CurrentPluginPanelRect() const { return m_CurrentPluginPanelRect; }
		// 「在内容浏览器中定位」:切到第三根(项目插件)并选中该插件的目录。
		// 引擎插件不在 `<项目根>/plugins` 下 ⇒ false + 可读理由(面板据此禁用该按钮)。
		bool LocatePluginInContentBrowser(const std::string& id, std::string* message = nullptr);
		// PLUG-AUTH-1:「新建插件…」向导入口 —— File ▸ New Plugin… 与插件管理器面板的
		// 「新建插件…」按钮走同一条路径(同一个脚手架/同一份模态状态)。
		// 启动器形态(没有项目)也可用:目标固定为引擎插件,Project 分段禁用并给理由。
		void OpenNewPluginModal(Wui::WuiContext& ctx);
		// ---- CPPT-7/PROJ-8:项目源码视图 + 外部 Visual Studio(内置编辑器只服务 Lua/Luau)----
		// 当前项目根(运行期;没有清单 —— 启动器/未打开项目 —— 返回空路径)。
		std::filesystem::path CurrentProjectRoot() const;
		// 解析一个 C++ 源码路径:绝对路径原样;`module:`(旧前缀)/相对路径依次按 项目根 → 仓库根 →
		// 内容根 解析。失败(false)= 可读原因写进 error。
		bool ResolveCppSourcePath(const std::string& path, std::filesystem::path& out,
			std::string* error) const;
		// 帧边界执行体:absolute → EditorLayer::OpenInVisualStudio;失败给可读提示 + 日志。
		void OpenInVisualStudioNow(const std::filesystem::path& absolute);
		// 动态脚本面板:从布局存档/AI ui.open 的 "script:<逻辑路径>" id 建实例。
		void EnsureScriptPanelFromId(const std::string& panelId);
		// 关闭动态面板(脚本编辑器工具栏 Close):与 Window 菜单同一条 TogglePanel 路径。
		void CloseEditorPanel(const std::string& panelId) override;
		// ---- W5-L1:文档场景外部改动提示(转发 EditorLayer)----
		bool ExternalSceneChanged() const override;
		void ReopenExternalScene() override;
		// P1b D5:内容浏览器双击 .wmodel → 实例化进当前文档场景。
		bool InstantiateModelFile(const std::string& logicalPath, std::string* message = nullptr) override;
		// D8a2:把项目渲染设置写回 project.we.yaml(Project Settings 面板的"保存")。
		bool SaveProjectRenderSettings(const Asset::RenderingSettings& settings,
			std::string* message = nullptr) override;
		// P4-1:物理设置写回 project.we.yaml(同一个面板的"保存")。
		bool SaveProjectPhysicsSettings(const Asset::PhysicsSettingsData& settings,
			std::string* message = nullptr) override;
		// P4-U4:资产导入默认值(project.we.yaml 的 imports:)。
		bool SaveProjectImportDefaults(const Asset::ModelImportSettings& settings,
			std::string* message = nullptr) override;
		bool SaveProjectStartupSettings(const std::string& renderer, const std::string& startScene,
			const std::string& contentRoot, std::string* message = nullptr) override;
		std::vector<std::string> ListProjectScenes() override;
		bool SaveProjectPackages(const std::vector<std::string>& packages, std::string* message = nullptr) override;
		// P4-UX15:把停靠面板切到前台(选中它所在标签页),供 AI 通道 ui.activate 用。
		bool AiActivatePanel(const std::string& panel, std::string* message);
		void ApplyProjectRendererChange(const std::string& renderer) override;
		// P1b D5:内容浏览器双击 .gltf/.glb → 导入(不实例化);回传 .wmodel 逻辑路径供打开预览。
		bool ImportModelFile(const std::string& sourcePath, std::string* message = nullptr,
			std::string* outLogicalModel = nullptr) override;
		// D10:导入到指定逻辑目录(内容浏览器当前文件夹 / 拖放命中文件夹)。
		bool ImportModelFileTo(const std::string& sourcePath, const std::string& destinationLogicalDir,
			std::string* message = nullptr, std::string* outLogicalModel = nullptr) override;
		// P1b D5:打开模型预览(每个 .wmodel 一个独立窗口;**只读预览,不改场景**)。
		void OpenModelPreview(const std::string& logicalPath) override;
		// P4-U13:prefab(内容浏览器双击 = 打开编辑;右键 = 实例化到当前场景)。
		void OpenPrefabEditor(const std::string& logicalPath) override;
		// P4-U13c:内容浏览器双击 .wprefab = 打开这个资产窗口(看/管理),编辑在窗口里的 Edit Prefab。
		void OpenPrefabWindow(const std::string& logicalPath) override;
		// 同一个入口的"带结果"版本(打开前后读一次盘):失败 = false + 可读原因,
		// 窗口仍然打开并在状态行写原因。AI 通道 `asset.open_prefab` 用它做失败语义。
		bool OpenPrefabWindowChecked(const std::string& logicalPath, std::string* message = nullptr);
		// P4-U13e:脚本化写 prefab **资产窗口**里的可编辑字段(与窗口控件同一条写入口):
		// panelId = "prefab:<逻辑路径>",component/field/axis 用面板小节里的短名。
		bool SetPrefabPanelField(const std::string& panelId, const std::string& component,
			const std::string& field, const std::string& value, const std::string& axis,
			std::string* message = nullptr);
		bool InstantiatePrefabAsset(const std::string& logicalPath, std::string* message = nullptr) override;
		// P4-U13d:创建预制体(层级面板的"Create Prefab from Selection…"与 AI 通道
		// asset.create_prefab 同一内核);实现全在 EditorLayer。
		bool CreatePrefabFromSelection(Entity root, const std::string& logicalPath, bool overwrite,
			std::string* message = nullptr) override;
		// P4-U13d:让内容浏览器选中(必要时先导航到)某个逻辑路径的资产;创建成功后由 EditorLayer 调用。
		bool SelectContentAsset(const std::string& logicalPath, const char* op) override;
		// ---- U25-M2:材质工作流(PanelHost 能力;转发 EditorLayer / 内容浏览器面板)----
		// 材质赋值:写进选中实体的 MeshRendererComponent.MaterialPath(失败给可读原因)。
		bool AssignMaterialToSelection(const std::string& logicalPath, Entity* outEntity,
			std::string* outPreviousPath, std::string* message = nullptr) override;
		// 撤销赋值 / Extract 赋回:把指定实体的 MaterialPath 写成给定值。
		bool SetEntityMaterialPath(Entity entity, const std::string& materialPath,
			std::string* message = nullptr) override;
		// 打开内容浏览器面板的"新建材质"向导(Window 菜单与材质面板的 Extract 共用这一条)。
		bool OpenNewMaterialWizard(bool fromSelection, std::string* message = nullptr) override;
		// 面板所在窗口的客户区原点(屏幕物理像素):跨窗口拖放命中用(见 Editor::AssetDropBridge)。
		bool PanelWindowScreenOrigin(const Wui::WuiContext& ctx, float* outX, float* outY) override;
		// P4-U13b:prefab 实例(属性面板实例条 + 层级右键菜单;实现全在 EditorLayer)。
		bool PrefabInstanceInfo(Entity entity, std::string* sourcePath, size_t* overrideCount,
			Entity* root) override;
		bool PrefabInstanceRevert(Entity root, std::string* message = nullptr) override;
		bool PrefabInstanceApply(Entity root, std::string* message = nullptr) override;
		bool PrefabInstanceUnpack(Entity root, std::string* message = nullptr) override;
		// P4-U13:prefab 编辑会话(横幅按钮 / AI 通道共用)。
		bool IsEditingPrefabDocument() const;
		bool SavePrefabDocument(std::string* message = nullptr);
		bool ClosePrefabDocument();
		// D10:把"选导入位置"交给内容浏览器(引擎内树状选择器,范围限定内容根内)。
		void RequestImportDestination(const std::string& sourcePath) override;
		// P4-UX16:面板级短提示统一进状态栏(见 PushNotice 的 4s/悬停冻结/移开宽限节奏)。
		void Notify(const std::string& message) override;
		// CPPT-3-FIX1:模块动作完成后的可见反馈(状态 + 引擎消息 + 迁移诊断计数与首条)——
		// 菜单项与 AI `module.reload` 走 EditorLayer 的同一条入口,这里只做一次提示文案组装。
		void NotifyCppModuleResult();
		// P4-U6b:面板级模态占用(属性面板的"添加组件"居中窗口)。
		void SetPanelModalOwner(const std::string& panelId) override { m_PanelModalOwner = panelId; }
		// W9-2 三层快捷键路由第 2 层:当前焦点面板。
		//   ① 主窗口处于"已附加面板"模式(顶栏标签)→ 该面板;
		//   ② 否则某个独立窗口在前台 → 该窗口的当前标签;
		//   ③ 否则 WUI 文本焦点所属面板(脚本编辑器/搜索框等);都没有 → nullptr。
		EditorPanel* FocusedPanel();
		// 上一帧(完整 UI 帧)结束时的文本焦点快照:UI 帧内判定 Ctrl+Z/Y 是否要让位给文本控件。
		bool TextFocusLatched() const { return m_TextFocusLatched; }
		// ---- 面板形态(单一事实源)----
		// 每个面板要么是普通停靠面板,要么是"独立窗口"(自带 OS 窗口 + 标签栏)。
		// 形态只在声明表里写一次,其余判定一律读 FormOf(),不再按面板名特判。
		enum class PanelForm { Docked, Independent };
		PanelForm FormOf(const std::string& panel) const;
		bool IsDeclaredPanel(const std::string& panel) const;
		bool IsIndependentPanel(const std::string& panel) const { return FormOf(panel) == PanelForm::Independent; }
		bool AttachSlotHighlighted() const override { return m_AttachSlotHighlight; }
		// W8:面板层拿到存档服务(由 EditorLayer 持有并注入场景)。
		Gameplay::SaveService* GetSaveService() override;
		// W8:脚本面板的三个宿主能力(转发给 EditorLayer;面板只依赖 PanelHost)。
		bool ScriptsReloadInstance(entt::entity handle, std::string* message) override;
		bool ScriptsOpenExternal(const std::string& logicalPath, std::string* message) override;
		bool ScriptsCreateFromTemplate(std::string& outLogicalPath, std::string* message) override;
		// Play/Simulate = 只读查看(用户 2026-09-15 确认:Play 下属性面板可查看不可改)。
		// 定义放 .cpp:本头文件只有 EditorLayer 的前置声明。
		bool IsReadOnlyMode() const override;
		// D7-1a:视口相机模式透传(视口面板的 2D/3D 切换按钮)。
		bool IsViewportCamera3D() const override;
		void ToggleViewportCamera3D() override;
		Wui::GizmoCamera GetGizmoCamera() const override;
		void AttachIndependentWindowToSlot(const std::string& panel) override;
		Entity PickEntityAt(glm::vec2 viewportLocal) override;
		EditorCamera& GetEditorCamera() override;
		Wui::GizmoOperation GetGizmoOperation() const override;
		// 开发/验证入口:触发层级面板第 index 行的真实点击回调
		// (WLD_HIERARCHY_CLICK 自动化用它复现"Play 下点层级行",不是绕过面板的旁路)。
		bool DebugClickHierarchyRow(size_t index);

	private:
		// 停靠
		void RenderNode(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area);
		void RenderTabs(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area);
		void RenderSplit(Wui::WuiContext& ctx, Wui::DockNode& node, const Wui::WuiRect& area);
		void RenderPanelContent(Wui::WuiContext& ctx, const std::string& id, const Wui::WuiRect& rect);
		// 浮动面板:在停靠区之上绘制,支持拖动/缩放/关闭与拖回停靠。
		void RenderFloating(Wui::WuiContext& ctx);
		// 独立 OS 窗口(每个 FloatWindowHost 一扇):**帧末**渲染,理由见实现处注释
		// (无障碍树的"当前窗口"与跨窗口落点登记都要求主窗口 UI 先画完)。
		void RenderIndependentWindows(Wui::WuiContext& ctx);
		// 单个"窗口内浮动面板"(停靠形态面板拖出后的形态):标题栏拖动、右下角缩放、
		// 关闭回停靠位。独立窗口(Independent)不走这里,它们有自己的 OS 窗口。
		void RenderFloatWindow(Wui::WuiContext& ctx, Wui::DockFloat& window, bool* closed);
		// 跨窗口标签拖拽:全局光标追踪 + 目标高亮 + 附加/新建/挂靠落点。
		// 挂靠栏:横跨主窗口的一条(类似菜单栏),独立窗口拖到其上即挂靠。
		void DrawAttachBar(Wui::WuiContext& ctx);
		// 独立窗口组件:创建/销毁(与停靠面板不同,各自拥有 OS 窗口)。
		// startHidden:创建后立刻隐藏(恢复为顶栏标签时用,避免窗口"闪一下就消失")。
		void AddFloatWindow(const std::string& panel, const Wui::WuiRect& screenRect, const char* origin,
			bool startHidden = false);
		// 整窗关闭(OS 窗口关闭或渲染失败):窗口内全部面板隐藏并写回布局。
		void CloseFloatWindow(const std::string& panel, bool recordChange, Wui::WuiContext* ctx);
		// 隐藏单个面板(标签栏 x 或 Window 菜单):窗口为空时销毁该窗口。
		void HideFloatPanel(const std::string& panel, Wui::WuiContext* ctx);
		// ---- 形态规则(T03)----
		// 加载/保存时按声明纠正布局:
		//  - 独立形态面板(以及未声明的历史面板)不得出现在停靠树与浮动记录里;
		//  - 停靠形态面板的"临时拖出"不跨会话(保存时回停靠树)。
		void StripIndependentPanelsFromTree(Wui::DockLayout& layout) const;
		void RestoreDockedPanelsFromFloat(Wui::DockLayout& layout) const;
		Wui::DockLayout LayoutForSave() const;
		// 停靠形态面板:关闭"临时浮动"时回到停靠树(而不是变成隐藏面板)。
		bool DockPanelBackToTree(const std::string& panel);
		// Window 菜单:打开/复用独立形态面板的窗口(已隐藏的窗口直接复用)。
		void OpenIndependentPanel(const std::string& panel);
		// D10:默认"打开 = 附加到主窗口"(用户 2026-09-19);显式分离/已保存的浮动记录不受影响。
		void OpenPanelAttached(const std::string& panel);
		// 独立窗口的屏幕矩形:优先用跨会话位置记忆,其次用声明表里的默认值。
		Wui::WuiRect FloatRectFor(const std::string& panel) const;
		// 承载指定面板的独立窗口查找入口(W7.3 跨窗口附加按它定位目标/源窗口)。
		// 跨窗口迁移配方:源/目标窗口用 FindFloatHost 定位,面板迁移用 AddPanel/RemovePanel,
		// 迁移后把该面板的 DockFloat.Rect 写成目标窗口 ScreenRect();源窗口为空则 EraseFloatHost。
		FloatWindowHost* FindFloatHost(const std::string& panel);
		// 从 m_FloatHosts 移除宿主,并清理其面板的每窗口屏幕位置缓存。
		void EraseFloatHost(FloatWindowHost* host);
		void SaveLayout();

		// 菜单 / 模态 / 布局
		void DrawMenuBar(Wui::WuiContext& ctx);
		// M4-S2:把本帧攒下的菜单栏下拉命令补画到 overlay 末尾(在所有面板与模态之后、
		// tooltip 之前调用一次)。没有菜单打开时是零成本。
		void FlushDeferredMenuDraws(Wui::WuiContext& ctx);
		// P4-U13:prefab 编辑横幅(顶部一条,标明"在改资产";右侧 保存 / 返回场景)。
		void DrawPrefabBar(Wui::WuiContext& ctx, float y);
		void DrawModals(Wui::WuiContext& ctx);
		// ---- P4-U13e:prefab 资产窗口的"未保存改动"守卫 ----
		// 关窗 / 进文档会话都会丢掉窗口里未落盘的编辑:先弹项目现成的确认模态(丢弃 / 取消),
		// 用户确认后才执行被延迟的动作。Intercept 返回 true = 本次动作已被拦下(等用户回答)。
		enum class PrefabPendingAction { None, Close, OpenDocument };
		PrefabPanel* PrefabPanelById(const std::string& panelId) const;
		bool InterceptPrefabUnsaved(const std::string& panelId, PrefabPendingAction action,
			const std::string& logicalPath = std::string());
		void RunPendingPrefabAction(Wui::WuiContext& ctx);
		void DrawPrefabUnsavedModal(Wui::WuiContext& ctx);
		std::string m_PrefabPendingPanel;
		std::string m_PrefabPendingLogical;
		PrefabPendingAction m_PrefabPendingAction = PrefabPendingAction::None;
		// 半路执行延迟动作时显式放行一次(否则刚点"丢弃"又会被守卫拦回来)。
		bool m_PrefabGuardBypass = false;
		// D10-10/D10-11(用户 2026-09-19):导入位置选择器是**窗口级模态** —— shell 自己持有
		// 源路径/选中目录/状态/展开集合/滚动;外框(居中/遮罩/标题栏/Esc)与按钮条走
		// World/WUI/Widgets/WuiModal.* 组件,整窗输入封锁由 OnRender 的 Begin/EndModalInputBlock 成对完成。
		// D10-15:DrawModals 里另外四个模态(unsaved/error/cooking/projectsettings)也走同一套
		// BeginModalFrame + ModalButtons/ModalFooter,并由同一组 Begin/EndModalInputBlock 挡输入。
		void RenderImportDestinationModal(Wui::WuiContext& ctx);
		// ---- CPPT-6-ED-NEWSCRIPT + PECS-T8:File ▸ New C++ …(组件 / 系统同一个向导)----
		// 菜单/内容浏览器入口 → 类型(组件默认 / 系统)+ 名称模态(合法 C++ 标识符 + 不重名,行内错误)
		// → 写**当前项目**模板:
		//   * 组件 → `<项目根>/src/Components/<Name>.h`(PROJ-8/T1,模板与行为逐字节不变);
		//   * 系统 → `<项目根>/src/Systems/<Name>.h`,并把配套的 include + Attach/Detach 登记
		//     自动写进 `<项目根>/src/GameProject.cpp`(WriteFileAtomically 的纯文本工具;
		//     幂等、认不出结构就一个字节都不写)。
		// 之后外部 Visual Studio 打开(帧边界;内置编辑器只服务 Lua/Luau)+ 状态栏提示;
		// 没有当前项目时不打开模态,直接给可读提示。操作日志与新建资产同口径。
		void OpenNewCppScriptModal(Wui::WuiContext& ctx);
		void DrawNewCppScriptModal(Wui::WuiContext& ctx);
		// 名称校验:没有项目 / 空 / 非法标识符 / 目标已存在 → 可读原因;空串 = 通过。
		std::string NewCppScriptNameError() const;
		// 目标绝对路径:组件 = `<当前项目根>/src/Components/<Name>.h`,
		// 系统 = `<当前项目根>/src/Systems/<Name>.h`(没有项目时为空路径的拼接)。
		std::filesystem::path NewCppScriptTargetPath() const;
		// 当前类型是否是"系统"(组件 = false)。类型下标见 m_NewCppScriptKind。
		bool NewCppScriptIsSystem() const;
		// 写模板 + (组件且项目类型账本存在时)登记类型 + (系统时)登记 GameProject.cpp +
		// 外部 VS 打开 + 状态栏提示 + 操作日志;失败写 m_NewCppScriptFailure 并返回 false。
		bool CreateNewCppScript(Wui::WuiContext& ctx);
		bool m_NewCppScriptOpen = false;
		uint32_t m_NewCppScriptOpenedFrame = 0;
		std::string m_NewCppScriptName;
		int m_NewCppScriptKind = 0;              // 0 = 组件(默认,既有自动化行为不变),1 = 系统
		std::string m_NewCppScriptFailure;      // 写盘失败原因(落点变化时清)
		std::string m_NewCppScriptFailureFor;   // 上面的原因对应的落点(变了就作废)

		// ---- PECS-T9:File ▸ New Lua System…(与 T8 那个 C++ 向导同源,但是**另一个**模态)----
		// 为什么另开而不是复用:`modal.newscript` 的落点/模板/校验类型全都不同(这里是
		// `<内容根>/scripts/systems/<Name>.luau` + Lua 文件名规则),复用会把两套状态搅在一起。
		// 骨架与 T8 一致:名称输入 + 实时落点回显 + 行内错误 + Enter/Esc,创建后在脚本编辑器里打开。
		void OpenNewLuaSystemModal(Wui::WuiContext& ctx);
		void DrawNewLuaSystemModal(Wui::WuiContext& ctx);
		// 名称校验:没有项目 / 没有内容根 / 空 / 非法文件名 / 目标已存在 → 可读原因;空串 = 通过。
		std::string NewLuaSystemNameError() const;
		// 目标绝对路径:`<内容根>/scripts/systems/<Name>.luau`。
		std::filesystem::path NewLuaSystemTargetPath() const;
		// 写模板(优先 <内容根>/scripts/templates/WorldScript.lua,缺失时内置骨架)+
		// 脚本编辑器打开 + 状态栏提示 + 操作日志;失败写 m_NewLuaSystemFailure 并返回 false。
		bool CreateNewLuaSystem(Wui::WuiContext& ctx);
		bool m_NewLuaSystemOpen = false;
		uint32_t m_NewLuaSystemOpenedFrame = 0;
		std::string m_NewLuaSystemName;
		std::string m_NewLuaSystemFailure;      // 写盘失败原因(落点变化时清)
		std::string m_NewLuaSystemFailureFor;   // 上面的原因对应的落点(变了就作废)

		// ---- PROJ-1/T1 + PROJ-7/T2:File ▸ New Project…(任意位置新建标准项目)----
		// 菜单入口 → 模态(项目名 + 位置 + Browse… + **模板选择** + 实时落点 + 行内错误)→
		// 生成骨架(ProjectScaffolder;清单走引擎 writer、启动场景来自模板或引擎序列化、
		// 模板来自 templates/project-*/)。
		// **原生对话框与落盘都在帧边界执行**(上一帧只置标记;渲染中途弹 Win32 模态/写盘会踩坑,
		// 与 m_PendingScriptOpen 同一条纪律)。成功后模态换成两个动作:在资源管理器打开 / 打开项目。
		// preselectTemplateId(PROJ-7/T3b,可选):打开时预选的模板 id(空 = 保持上次选择/默认 empty)。
		// 启动器"新建示例项目…"用它把 example 模板带进向导;该模板不存在时按既有回退规则
		// (empty → 第一个模板)降级,不额外报错。
		void OpenNewProjectModal(Wui::WuiContext& ctx, const std::string& preselectTemplateId = std::string());
		void DrawNewProjectModal(Wui::WuiContext& ctx);
		// 模板库刷新(打开模态时强制刷;模态开着时按 2s 节流刷 —— 模板是磁盘事实,
		// 但**不**每帧扫目录)。选中项被删时退回第一个模板。
		void RefreshNewProjectTemplates(bool force);
		// 当前选中的模板(找不到 = nullptr;调用方先 RefreshNewProjectTemplates)。
		const Editor::ProjectScaffolder::TemplateInfo* SelectedNewProjectTemplate() const;
		// 帧边界执行体(OnRender 开头消费标记)。
		void RunNewProjectBrowse(Wui::WuiContext& ctx);
		void RunNewProjectCreate(Wui::WuiContext& ctx);
		// 校验(行内错误):名称 / 落点 / 模板;空串 = 通过。全部走 ProjectScaffolder 的只读校验。
		std::string NewProjectNameError() const;
		std::string NewProjectLocationError() const;
		// 实时落点(<位置>/<名称>)与规范化后的名称。
		std::filesystem::path NewProjectTargetRoot() const;
		std::string NewProjectTrimmedName() const;
		bool m_NewProjectOpen = false;
		uint32_t m_NewProjectOpenedFrame = 0;
		std::string m_NewProjectName;            // 项目名(= 目录名)
		std::string m_NewProjectLocation;        // 位置(UTF-8,来自输入框或原生文件夹对话框)
		std::string m_NewProjectFailure;         // 落盘失败原因(名称/落点变化时清)
		std::string m_NewProjectFailureFor;      // 上面的原因对应的落点(变了就作废)
		bool m_NewProjectBrowsePending = false;  // 下一帧开头弹"选择项目位置"原生对话框
		bool m_NewProjectCreatePending = false;  // 下一帧开头落盘
		bool m_NewProjectCreated = false;        // 成功态(模态画两个动作按钮)
		std::filesystem::path m_NewProjectRoot;  // 成功后的项目根(绝对)
		std::string m_NewProjectCreatedName;
		// PROJ-7/T2:模板选择。模板库 = templates/project-*/template.json(ListTemplates 扫描);
		// 默认选 empty(没有 empty 就选第一个模板)。破坏性/失败原因按"选中模板"失效。
		std::vector<Editor::ProjectScaffolder::TemplateInfo> m_NewProjectTemplates;
		double m_NewProjectTemplatesScannedAt = 0.0;   // steady_clock 秒;0 = 本会话还没扫过
		std::string m_NewProjectTemplateId = "empty";  // 当前选中的模板 id
		std::string m_NewProjectCreatedTemplateName;   // 成功态:实际用的模板名
		std::string m_NewProjectCreatedStartScene;     // 成功态:清单里的 start_scene(相对内容根)
		// PROJ-2/T1:P4 —— 向导复选框"包含最小可运行场景(相机 + 方向光)",默认勾选;
		// 创建结果按创建时的勾选快照进成功态(提示文案 / 交给 ProjectScaffolder 的参数)。
		// PROJ-7/T2:所选模板声明了 defaultScene(自带启动场景)时该勾选框禁用(灰显 + 理由)。
		bool m_NewProjectStarterScene = true;
		bool m_NewProjectCreatedStarterScene = true;
		// PROJ-3/T1(P2b):本次创建写进项目根的启动入口(相对文件名,已排序;
		// 空 = exe 与 .cmd 都没写成功 —— 成功态不画"启动入口"那一行)。
		std::vector<std::string> m_NewProjectEntryPoints;

		// ---- PLUG-AUTH-1:File ▸ New Plugin…(插件向导)----
		// 菜单/面板入口 → 模态(目标引擎/项目 + 6 类模板 + 名称/插件 ID + 实时落点 + 行内错误)
		// → 生成骨架(PluginScaffolder;模板来自 templates/plugin-*/)→ 成功态给出落盘目录、
		// 「在内容浏览器中定位」(仅项目插件)与「打开插件目录」。
		// 落盘同样在**帧边界**执行(上一帧只置标记,与 New Project 同一条纪律)。
		void DrawNewPluginModal(Wui::WuiContext& ctx);
		// 模板库刷新(打开模态时强制刷;模态开着时按 2s 节流刷 —— 模板是磁盘事实)。
		// 选中项被删时退回"当前目标允许的第一个模板"。
		void RefreshNewPluginTemplates(bool force);
		// 只按当前目标重选模板 id(**不重建模板向量**)—— 目标分段按钮的点击发生在帧中途,
		// 而同一帧后面还持有指向 m_NewPluginTemplates 的指针;在那里重建向量会让指针悬垂。
		void EnsureNewPluginTemplateSelection();
		const Editor::PluginScaffolder::PluginTemplateInfo* SelectedNewPluginTemplate() const;
		// 帧边界执行体(OnRender 开头消费标记)。
		void RunNewPluginCreate(Wui::WuiContext& ctx);
		// 校验(行内错误):名称 / 插件 ID / 目标落点 / 模板;空串 = 通过。
		std::string NewPluginNameError() const;
		std::string NewPluginIdError() const;
		std::string NewPluginTargetError() const;
		std::string NewPluginTemplateError() const;
		// 目标插件根:引擎 = <repo>/plugins;项目 = <项目根>/plugins(没有项目时为空)。
		std::filesystem::path NewPluginPluginsRoot() const;
		// 实时落点 = <插件根>/<名称>(绝对)。
		std::filesystem::path NewPluginTargetRoot() const;
		std::string NewPluginTrimmedName() const;
		std::string NewPluginTrimmedId() const;
		bool m_NewPluginOpen = false;
		uint32_t m_NewPluginOpenedFrame = 0;
		Editor::PluginScaffolder::PluginTarget m_NewPluginTarget =
			Editor::PluginScaffolder::PluginTarget::Engine;
		std::string m_NewPluginTemplateId = "empty";
		std::string m_NewPluginName;
		std::string m_NewPluginId;
		bool m_NewPluginIdTouched = false;      // 用户改过 ID ⇒ 名称变化不再自动改写 ID
		std::string m_NewPluginFailure;         // 落盘失败原因(名称/ID/目标变化时清)
		std::string m_NewPluginFailureFor;      // 上面的原因对应的落点(变了就作废)
		bool m_NewPluginCreatePending = false;  // 下一帧开头落盘
		bool m_NewPluginCreated = false;        // 成功态(模态画动作按钮)
		std::filesystem::path m_NewPluginRoot;  // 成功后的插件根(绝对)
		std::vector<std::string> m_NewPluginFiles;
		std::string m_NewPluginCreatedId;
		std::string m_NewPluginCreatedTarget;   // engine / project(成功态显示)
		std::vector<Editor::PluginScaffolder::PluginTemplateInfo> m_NewPluginTemplates;
		double m_NewPluginTemplatesScannedAt = 0.0;   // steady_clock 秒;0 = 本会话还没扫过

		// PLUG-AUTH-1:把任意目录在内容浏览器的某个根里定位(定位动作与插件面板共用;
		// 失败 = false + 可读原因)。
		bool RevealPathInContentBrowserPanel(ContentBrowserPanel::RootScope scope,
			const std::filesystem::path& absolutePath, std::string* message);

		// ---- PROJ-11/T1:File ▸ 生成项目构建入口(给已存在项目补 CMake/build.cmd/启动器)----
		// 动作本身走 ProjectScaffolder::EnsureBuildEntryPoints(纯逻辑 + 落盘),结果模态
		// 显示"补了什么/跳过了什么" + 可复制的两条指引命令。
		void DrawBuildEntryModal(Wui::WuiContext& ctx);
		bool m_BuildEntryOpen = false;
		Editor::ProjectScaffolder::BuildEntryResult m_BuildEntryResult;

		// ---- PROJ-2/T1:项目启动器 + File ▸ Open Project… + 运行 ▸ 启动项目(Runtime)----
		// 启动决策(是否显示启动器)在 EditorLayer;这里只渲染最近列表与四个动作、登记 a11y。
		void DrawProjectLauncherModal(Wui::WuiContext& ctx);
		// PROJ-3/T1:纯启动器模式的整页渲染(启动器页 + 新建项目向导 + 错误模态;
		// 没有菜单/停靠区/状态栏/独立窗口)。由 OnRender 在 m_LauncherMode 下调用。
		void RenderLauncherPage(Wui::WuiContext& ctx);
		// "打开项目…":选目录(帧边界原生对话框)→ 校验 project.we.yaml → RelaunchWithProject。
		void RunOpenProjectBrowse(Wui::WuiContext& ctx);
		bool m_OpenProjectBrowsePending = false;   // 下一帧开头弹"选择项目目录"
		// 最近项目缓存(避免逐帧查盘;列表可见时 1 秒节流刷新,见实现)。
		void RefreshRecentProjectsIfStale(bool force = false);
		const std::vector<Editor::RecentProjectEntry>& RecentProjects();
		std::vector<Editor::RecentProjectEntry> m_RecentProjects;
		double m_RecentProjectsLoadedAt = -1.0e9;
		// PROJ-4/T1(P1/P2):启动器搜索词 + 最近列表滚动位置(过滤只影响"画哪些行",
		// 行 id / 打开目标 / 删除目标一律用 m_RecentProjects 的原下标,避免筛选后错位)。
		std::string m_LauncherSearch;
		float m_LauncherScrollY = 0.0f;
		// PROJ-5R/T1(v2):最近项目"删除…"的**一步**确认模态(打开 = true;不再输入目录名)。
		// 打开时把**目标路径与显示名快照下来** —— 之后的列表刷新/筛选/滚动不会让目标串位;
		// 真正的删除只走 ProjectLauncher::DeleteProjectPermanently(全部守卫在那边);
		// 目标目录已不存在时,同一个模态换成"从列表移除"(只动最近列表,不碰磁盘)。
		bool m_LauncherDeleteOpen = false;
		std::string m_LauncherDeletePath;
		std::string m_LauncherDeleteName;
		std::string m_LauncherDeleteError;
		// PROJ-7/T3b:启动器模态"刚回到前台"的那一帧吞掉输入 —— 向导(新建项目)的
		// 取消/关闭与启动器按钮在同一帧收口时,上一帧的点击/Esc 会与启动器按钮的
		// 屏幕矩形重叠,同帧判定会把"取消向导"误判成"点了启动器按钮"(实测:取消后
		// 同帧又开了一次向导,op 日志同一帧两条;Esc 在启动器模式下会把整个进程退掉)。
		// 只在"不可见 → 可见"的过渡帧生效,之后正常响应。
		bool m_LauncherModalVisible = false;
		void DrawLauncherDeleteFlow(Wui::WuiContext& ctx, const Wui::WuiRect& frame, bool escapePressed);
		// 运行 ▸ 启动项目(Runtime):目标 = 当前项目根(EditorLayer完成定位/启动/日志)。
		void LaunchCurrentProjectRuntime();
		// 启动器的"打开项目…"入口(与 File ▸ Open Project… 同一条帧边界路径)。
		void RequestOpenProjectBrowse(Wui::WuiContext& ctx);

		// 打开模态时扫一遍当前内容根(World::Paths::AssetRoot())的目录树(低频操作;权限错误跳过)。
		void ScanImportTree();
		void RestoreLayout(const std::string& json);
		void RecordDockChange(Wui::WuiContext& ctx, const std::string& action, const std::string& target, const std::string& before);
		void TogglePanel(Wui::WuiContext& ctx, const std::string& panel);
		void ResetLayout(Wui::WuiContext& ctx);
		// P4-UX1:面板标题走本地化(标签 / Window 菜单 / 挂靠标签共用同一来源)。
		std::string PanelTitle(const std::string& id) const;
		// P4-UX6:主窗口底部状态栏(场景/选择/后端/帧率),同时登记无障碍节点 `shell.status`。
		void DrawStatusBar(Wui::WuiContext& ctx, const Wui::WuiRect& rect);
		// CPPT-3:Game 模块热重载状态的中文/英文显示文案(状态栏 + 菜单动作反馈共用;
		// 稳定字面量在 EditorLayer::CppModuleStateName,这里只做显示)。
		std::string CppModuleStateText() const;
		std::string CppModuleHintText() const;

		EditorLayer& m_Editor;
		// PROJ-3/T1:纯启动器模式(构造参数;不读布局存档、不恢复独立窗口、只画启动器页)。
		bool m_LauncherMode = false;
		Wui::DockLayout m_Layout;
		std::filesystem::path m_LayoutPath;
		std::vector<std::string> m_Panels;
		Wui::WuiTheme m_Theme;
		// P4-UX1:主题模式变化(暗/浅/跟随系统)时按代刷新 m_Theme。
		uint32_t m_ThemeGeneration = 0;
		std::unordered_map<std::string, std::unique_ptr<EditorPanel>> m_PanelRegistry;
		Wui::WuiRect m_ViewportRect;
		// PLUG-T3b:插件扩展宿主(命令/面板注册表 + 面板 Draw 的 WUI 小组件表)。
		std::unique_ptr<PluginEditorHost> m_PluginEditorHost;
		// 正在渲染的插件面板矩形(仅 PluginPanel::OnRender → DrawPluginPanel 期间有效;
		// PluginEditorHost 的 Draw 桥据此拿到控件布局区域)。
		Wui::WuiRect m_CurrentPluginPanelRect { 0, 0, 0, 0 };

		bool m_SplitterDragging = false;
		Wui::DockNode* m_DragSplitNode = nullptr;
		bool m_DragSplitRow = true;
		std::string m_SplitterBeforeJson;
		std::string m_DropTargetPanel;
		Wui::DropZone m_DropZone = Wui::DropZone::Center;
		// 编辑器级四边停靠:拖拽面板到窗口边缘时激活(优先于面板内的分栏落区)。
		bool m_EdgeDockActive = false;
		Wui::DropZone m_EdgeDropZone = Wui::DropZone::Center;
		std::string m_LastDragTarget;
		Wui::DropZone m_LastDragZone = Wui::DropZone::Center;
		// 独立窗口的"上次位置尺寸"记忆:收回停靠后再拖出沿用用户调好的尺寸。
		std::unordered_map<std::string, Wui::WuiRect> m_LastFloatRects;
		// 停靠面板"上次所在标签组"记忆(组的首个面板 id):Window 菜单重新打开时回到原组,
		// 而不是固定锚到 FirstPanel。每帧由 RenderTabs 按实际停靠树刷新。
		std::unordered_map<std::string, std::string> m_LastDockAnchors;
		// 挂靠槽位:屏幕矩形(每帧计算)与高亮状态。
		Wui::WuiRect m_AttachSlotScreenRect;
		bool m_AttachSlotHighlight = false;
		float m_AttachBarHeight = 26.0f;
		// 本帧的落点预览:画在浮动面板之上。拖动中的浮动面板正好盖在目标上,
		// 预览若跟停靠区一起画就会被它挡住(用户看不到"会落到哪")。
		Wui::WuiRect m_DropPreviewRect;
		bool m_DropPreviewActive = false;
		// 标签 × 的关闭请求:渲染遍历期间不能动停靠树(关闭组内最后一个标签会让该组
		// 塌缩,调用方持有的 children 引用随即失效)——统一在遍历结束后执行。
		std::vector<Wui::PanelId> m_PendingPanelCloses;
		// W9-2 修复:面板渲染中途请求打开脚本编辑器(内容浏览器双击/Scripts 按钮)——
		// 建新窗口(新 Vulkan 交换链)+附加标签不能在帧内做,统一推迟到下一帧开头。
		std::vector<std::string> m_PendingScriptOpen;
		// 已附加到主窗口的独立窗口(标签切换关系):"" = 主界面。
		std::vector<std::string> m_AttachedPanels;
		std::string m_ActiveWindowTag;
		// W9-2:上一帧结束时的"WUI 有文本焦点"快照(见 TextFocusLatched)。
		bool m_TextFocusLatched = false;
		// 附加标签的拖动状态:按下 / 正在拖 / 按下位置。
		std::string m_AttachTagPress;
		std::string m_AttachTagDrag;
		glm::vec2 m_AttachTagPressPos { 0, 0 };
		int m_AttachCooldownFrames = 0; // 挂靠后短暂抑制"拖出",避免同一次拖拽再次浮出
		std::string m_TabDragPanel;     // 本次拖拽真正起手于哪个标签页(tab 按下)
		// P4-UX9:拖动独立窗口 = 预置位置 + 系统移动循环(SC_MOVE) + 落点判定,见实现注释。
		void PerformIndependentWindowDrag(const std::string& panel);
		// ---- P4-UX10:启动恢复上次开着的独立窗口 ----
		struct PendingFloatRestore
		{
			std::vector<std::string> Panels;   // 同一窗口的标签页(首标签 = 窗口 key)
			Wui::WuiRect Rect { 0, 0, 0, 0 };
			bool Attached = false;             // 上次是挂靠(chip)还是浮窗
		};
		// forceTabs = 一律恢复成顶栏标签(不弹窗口);否则按每项的 Attached 决定。
		void RestoreIndependentWindows(const std::vector<PendingFloatRestore>& items, bool forceTabs);
		std::vector<PendingFloatRestore> m_PendingFloatRestore;   // Ask 模式:等用户回答
		bool m_RestoreAskRemember = false;                        // 询问框里的"记住我的选择"

		// 可超时的非模态提示(人类交互:悬停暂停计时 / 移出给宽限再淡出)。
		// 状态栏提示与"恢复询问条"共用这一套节拍,时间参数各自传。
		struct TimedNotice
		{
			double ShownAt = 0.0;
			double LeaveAt = 0.0;     // 悬停结束的时刻(0 = 没离开过)
			float Alpha = 1.0f;
			bool Hovered = false;
			// 返回 false = 这次该收起来了(调用方负责关闭)。
			bool Update(double now, bool hovered, double staySeconds, double graceSeconds, double fadeSeconds);
		};
		struct StatusNotice
		{
			std::string Text;
			TimedNotice Timing;
			bool Active = false;
		};
		StatusNotice m_Notice;
		// 恢复询问条的节拍(与状态栏提示同一套规则,只是停留更久:那是个需要阅读的决定)。
		TimedNotice m_RestorePromptTiming;
		void PushNotice(const std::string& text);
		void DrawStatusNotice(Wui::WuiContext& ctx, const Wui::WuiRect& statusBar);
		// 启动询问:贴在状态栏上方的一条非模态通知(不压暗、不挡操作)。
		Wui::WuiRect RestorePromptRect(const Wui::WuiRect& statusBar) const;
		void DrawRestorePrompt(Wui::WuiContext& ctx, const Wui::WuiRect& statusBar);
		// 上一帧各独立窗口的位置(用于判断"停稳在槽位上")。
		std::unordered_map<std::string, Wui::WuiRect> m_LastFloatScreenRects;

		// 面板拖拽/浮动状态。
		std::string m_DragPanel;            // 本帧拖拽中的面板(来自 payload "panel:")
		glm::vec2 m_LastDragPos { 0, 0 };   // 拖拽结束位置(浮动窗口落点)
		std::string m_MovingFloat;          // 正在移动的浮动面板
		glm::vec2 m_FloatGrabOffset { 0, 0 };
		std::string m_FloatResize;          // 正在缩放的浮动面板
		glm::vec2 m_FloatResizeStart { 0, 0 };
		Wui::WuiRect m_FloatResizeRect;
		std::string m_FloatChangeBefore;    // 浮动移动/缩放前的布局快照(操作日志)
		std::string m_BringFloatFront;      // 本帧请求置顶的浮动面板(下一帧生效)
		std::vector<std::unique_ptr<FloatWindowHost>> m_FloatHosts;

		// ---- P3-1①:面板拖拽状态归零(唯一出口)----
		// ClearPanelDragIdentity:清"这次拖拽是谁"的成员(起手标签/跟随窗口/落点位置)——
		// 它们是"面板刚回到停靠树就再次浮出"的原料,松手那一帧也必须清;
		// ClearPanelDragState:在上面基础上再清落点成员(目标面板/四边高亮/预览)与挂靠标签
		// 拖拽 —— 落点成员要留到下一帧 AcceptDrop 消费,所以只有"拖拽确定结束"时(落点已消费、
		// Esc 取消)才用这个完整版本;
		// EndPanelDrag:完整版本 + WuiContext 的 dragging/payload/drop 标记一起清。
		void ClearPanelDragIdentity();
		void ClearPanelDragState();
		void EndPanelDrag(Wui::WuiContext& ctx);
		// 面板当前形态(attach/detach 的幂等判定 + state.dump 的 "attach" 段 +
		// AttachTag 无障碍节点的 value):独立形态 = attached / floating / hidden;
		// 停靠形态 = docked / floating(主窗口内临时浮动)/ hidden。
		const char* PanelStateLabel(const std::string& panel) const;

		// 菜单栏(保留模式树)。
		std::shared_ptr<Wui::WuiBox> m_MenuBar;
		std::shared_ptr<Wui::WuiButton> m_FileButton;
		// P4-U6b:View 菜单(相机模式/预览 + 范围可视化开关)。
		std::shared_ptr<Wui::WuiButton> m_WindowButton;
		// PROJ-2/T1:运行菜单(运行 ▸ 启动项目(Runtime))。
		std::shared_ptr<Wui::WuiButton> m_RunButton;
		// P4-U6b:当前显示帧级模态的面板(空 = 无)。帧初封锁输入,渲染该面板前解开。
		std::string m_PanelModalOwner;
		Wui::WuiId m_OpenMenu = 0;
		Wui::WuiRect m_MenuHeaderRect;
		// M4-S2:`菜单栏下拉`的延后绘制批次(与 U29 的 WuiDeferredPopupScope 同一口径:
		// 绘制搬到帧末,命中/遮挡登记留在原地)。帧末由 FlushDeferredMenuDraws 追加到 overlay。
		std::vector<Wui::WuiDrawCommand> m_DeferredMenuCommands;
		Wui::WuiContext* m_Ctx = nullptr;

		// D10-10:导入位置模态的状态与目录树行(打开时重建;根行 Depth 0,其余按路径排序)。
		struct ImportTreeRow
		{
			std::filesystem::path Path;
			int Depth = 0;
			bool HasChildren = false;
		};
		bool m_ImportModalOpen = false;
		std::filesystem::path m_ImportTreeRoot;   // 当前内容根(World::Paths::AssetRoot()),打开模态时写入
		std::filesystem::path m_ImportSourcePath; // 已选好的源文件(来自 File ▸ Import glTF...)
		std::filesystem::path m_ImportDestDir;    // 选中目录(默认 = 打开时的内容浏览器当前文件夹)
		std::string m_ImportStatus;               // 状态行附加文本(成功/失败的可读信息)
		std::set<std::filesystem::path> m_ImportTreeOpen; // 模态自己的树展开集合
		std::vector<ImportTreeRow> m_ImportTree;
		float m_ImportTreeScroll = 0.0f;
	};
}
