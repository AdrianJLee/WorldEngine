#pragma once

#include "Panels/ContentBrowserPanel.h"
#include "Panels/HierarchyPanel.h"
#include "Panels/InputMapPanel.h"
#include "Panels/PropertiesPanel.h"
#include "Panels/ReadoutPanels.h"
#include "Panels/SavePanel.h"
#include "Panels/LevelPanel.h"
#include "Panels/MaterialEditorPanel.h"
#include "Panels/ScriptEditorPanel.h"
#include "Panels/ScriptsPanel.h"
#include "Panels/ViewportPanel.h"
#include "Panels/WidgetGalleryPanel.h"
#include "Panels/WindowsPanel.h"
#include "Panels/AttachSlotPanel.h"
#include "FloatWindowHost.h"

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

	// 编辑器外壳:停靠布局、菜单栏、模态与面板注册表。业务面板已组件化到
	// Panels/ 目录,外壳只负责驱动它们渲染并通过 PanelHost / ViewportHost 供给能力。
	class EditorShell : public ViewportHost
	{
	public:
		explicit EditorShell(EditorLayer& editor);
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
		// W9-2:打开脚本编辑器(每个脚本一个 "script:<逻辑路径>" 面板,创建后默认附加到主窗口)。
		void OpenScriptEditor(const std::string& logicalPath) override;
		// 帧边界执行版:内部使用(AI 通道在帧首、OnRender 开头处理待办时)。
		void OpenScriptEditorNow(const std::string& logicalPath);
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
		// D10:把"选导入位置"交给内容浏览器(引擎内树状选择器,范围限定内容根内)。
		void RequestImportDestination(const std::string& sourcePath) override;
		// P4-UX16:面板级短提示统一进状态栏(见 PushNotice 的 4s/悬停冻结/移开宽限节奏)。
		void Notify(const std::string& message) override;
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
		void DrawModals(Wui::WuiContext& ctx);
		// D10-10/D10-11(用户 2026-09-19):导入位置选择器是**窗口级模态** —— shell 自己持有
		// 源路径/选中目录/状态/展开集合/滚动;外框(居中/遮罩/标题栏/Esc)与按钮条走
		// World/WUI/Widgets/WuiModal.* 组件,整窗输入封锁由 OnRender 的 Begin/EndModalInputBlock 成对完成。
		// D10-15:DrawModals 里另外四个模态(unsaved/error/cooking/projectsettings)也走同一套
		// BeginModalFrame + ModalButtons/ModalFooter,并由同一组 Begin/EndModalInputBlock 挡输入。
		void RenderImportDestinationModal(Wui::WuiContext& ctx);
		// 打开模态时扫一遍内容根(WLD_ASSETPATH)的目录树(低频操作;权限错误跳过)。
		void ScanImportTree();
		void RestoreLayout(const std::string& json);
		void RecordDockChange(Wui::WuiContext& ctx, const std::string& action, const std::string& target, const std::string& before);
		void TogglePanel(Wui::WuiContext& ctx, const std::string& panel);
		void ResetLayout(Wui::WuiContext& ctx);
		// P4-UX1:面板标题走本地化(标签 / Window 菜单 / 挂靠标签共用同一来源)。
		std::string PanelTitle(const std::string& id) const;
		// P4-UX6:主窗口底部状态栏(场景/选择/后端/帧率),同时登记无障碍节点 `shell.status`。
		void DrawStatusBar(Wui::WuiContext& ctx, const Wui::WuiRect& rect);

		EditorLayer& m_Editor;
		Wui::DockLayout m_Layout;
		std::filesystem::path m_LayoutPath;
		std::vector<std::string> m_Panels;
		Wui::WuiTheme m_Theme;
		// P4-UX1:主题模式变化(暗/浅/跟随系统)时按代刷新 m_Theme。
		uint32_t m_ThemeGeneration = 0;
		std::unordered_map<std::string, std::unique_ptr<EditorPanel>> m_PanelRegistry;
		Wui::WuiRect m_ViewportRect;

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
		// P4-U6b:当前显示帧级模态的面板(空 = 无)。帧初封锁输入,渲染该面板前解开。
		std::string m_PanelModalOwner;
		Wui::WuiId m_OpenMenu = 0;
		Wui::WuiRect m_MenuHeaderRect;
		Wui::WuiContext* m_Ctx = nullptr;

		// D10-10:导入位置模态的状态与目录树行(打开时重建;根行 Depth 0,其余按路径排序)。
		struct ImportTreeRow
		{
			std::filesystem::path Path;
			int Depth = 0;
			bool HasChildren = false;
		};
		bool m_ImportModalOpen = false;
		std::filesystem::path m_ImportTreeRoot;   // 内容根(WLD_ASSETPATH),打开模态时写入
		std::filesystem::path m_ImportSourcePath; // 已选好的源文件(来自 File ▸ Import glTF...)
		std::filesystem::path m_ImportDestDir;    // 选中目录(默认 = 打开时的内容浏览器当前文件夹)
		std::string m_ImportStatus;               // 状态行附加文本(成功/失败的可读信息)
		std::set<std::filesystem::path> m_ImportTreeOpen; // 模态自己的树展开集合
		std::vector<ImportTreeRow> m_ImportTree;
		float m_ImportTreeScroll = 0.0f;
	};
}
