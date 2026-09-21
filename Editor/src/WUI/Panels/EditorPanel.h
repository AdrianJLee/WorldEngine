#pragma once

#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiGizmo.h"
#include "World/Core/Asset/ProjectManifest.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

#include <cstdint>
#include <string>

namespace World
{
	class EditorLayer;
	namespace Gameplay { class SaveService; }

	// 面板间协作窄接口:面板只依赖这些能力,不依赖 EditorLayer 全部。
	class PanelHost
	{
	public:
		virtual ~PanelHost() = default;
		virtual Ref<Scene> GetActiveScene() = 0;
		virtual Entity GetSelectedEntity() = 0;
		virtual void SetSelectedEntity(Entity entity) = 0;
		virtual void MarkDocumentDirty() = 0;
		virtual void DuplicateSelectedEntity() = 0;
		virtual void OpenScene(const std::filesystem::path& path) = 0;
		virtual Wui::WuiTheme& Theme() = 0;
		// 图标纹理 id(与 ViewportHost 一致;0 = 无图标)。
		virtual uint64_t GetIconId(int index) const = 0;
		// 旧式(GL)纹理纪元:窗口/上下文重建后自增,面板据此重新加载自己的图标。
		virtual uint32_t TextureEpoch() const = 0;
		// ---- 独立窗口(与停靠面板不同的组件)----
		virtual size_t IndependentWindowCount() const = 0;
		virtual std::string IndependentWindowPanel(size_t index) const = 0;
		// 该窗口承载的全部标签标题(连接),窗口管理器显示用。
		virtual std::string IndependentWindowLabel(size_t index) const = 0;
		virtual void FocusIndependentWindow(const std::string& panel) = 0;
		// 关闭独立窗口并把面板作为停靠标签恢复。
		virtual void DockBackIndependentWindow(const std::string& panel) = 0;
		// ---- 挂靠槽位(主窗口接收独立窗口的位置)----
		virtual bool AttachSlotHighlighted() const = 0;
		// 把独立窗口挂靠到槽位:面板进入槽位所在标签组,OS 窗口销毁。
		virtual void AttachIndependentWindowToSlot(const std::string& panel) = 0;
		// W8:存档服务(宿主注入;未就绪时返回 nullptr,面板据此显示提示)。
		virtual Gameplay::SaveService* GetSaveService() { return nullptr; }
		// Play/Simulate 期间为 true:面板只读查看(可选中/显示,但不改场景数据)。
		virtual bool IsReadOnlyMode() const { return false; }
		// D7-1a:视口相机模式(2D/3D)与切换 —— 视口面板用它画切换按钮。
		virtual bool IsViewportCamera3D() const { return false; }
		virtual void ToggleViewportCamera3D() {}
		// D7-1b:gizmo 用的相机摘要(按当前视口模式由宿主提供 2D 或 3D 相机)。
		virtual Wui::GizmoCamera GetGizmoCamera() const { return Wui::GizmoCamera {}; }
		// D3:在材质编辑器里打开指定 .wmat(内容浏览器双击/Window 菜单共用);
		// 未注册材质面板时为空实现(不影响其它面板)。
		virtual void OpenMaterialEditor(const std::string& path) {}
		// ---- W8:脚本面板(Panels/ScriptsPanel)协作;默认空实现 = 宿主未接线 ----
		// P4-U13:prefab 资产(内容浏览器双击/右键共用)。
		// 打开编辑:把 .wprefab 当**文档**打开(顶部横幅标明"正在编辑 Prefab",保存 = 写回资产)。
		virtual void OpenPrefabEditor(const std::string& logicalPath) { (void)logicalPath; }
		// P4-U13c:打开 prefab **资产窗口**(看/管理:实体树 + 组件摘要 + 引用资产 + 场景实例)。
		// 与 `.wmodel` 预览同款:创建后默认附加到主窗口。编辑仍走 OpenPrefabEditor(文档会话)。
		virtual void OpenPrefabWindow(const std::string& logicalPath) { (void)logicalPath; }
		// 实例化到当前场景(世界原点;成功时选中实例根并标脏)。失败给出可读原因。
		virtual bool InstantiatePrefabAsset(const std::string& logicalPath, std::string* message = nullptr)
		{
			(void)logicalPath;
			if (message) *message = "prefab instantiate is not wired to a host";
			return false;
		}
		// ---- P4-U13b:prefab 实例(层级徽标 + 属性面板实例条共用)----
		// 选中实体属于哪个实例(实例根,或实例子树内的成员都算)→ 来源路径 / 覆盖计数 / 实例根。
		// 返回 false = 该实体不属于任何实例(面板据此不画实例条)。
		virtual bool PrefabInstanceInfo(Entity entity, std::string* sourcePath, size_t* overrideCount,
			Entity* root)
		{
			(void)entity;
			(void)sourcePath;
			(void)overrideCount;
			(void)root;
			return false;
		}
		// 回滚到资产:重新读来源 .wprefab,把实例子树的组件值写回(覆盖记录随之清空)。
		virtual bool PrefabInstanceRevert(Entity root, std::string* message = nullptr)
		{
			(void)root;
			if (message) *message = "prefab revert is not wired to a host";
			return false;
		}
		// 应用到资产:把实例子树写回来源 .wprefab(会覆盖资产),并清空覆盖记录。
		virtual bool PrefabInstanceApply(Entity root, std::string* message = nullptr)
		{
			(void)root;
			if (message) *message = "prefab apply is not wired to a host";
			return false;
		}
		// 断开链接(Unpack):实体保持原样但不再是实例(之后不再跟随资产、也不再登记覆盖)。
		virtual bool PrefabInstanceUnpack(Entity root, std::string* message = nullptr)
		{
			(void)root;
			if (message) *message = "prefab unpack is not wired to a host";
			return false;
		}
		// 重载一个场景脚本实例。实现必须复用 EditorLayer::ReloadLuaScriptComponent ——
		// 与属性面板 Reload 按钮、帧边界轮询、AI script.reload 同一条入口,面板不自己编排重载。
		virtual bool ScriptsReloadInstance(entt::entity handle, std::string* message = nullptr)
		{
			(void)handle;
			if (message) *message = "scripts panel reload is not wired to a host";
			return false;
		}
		// 用系统默认程序打开磁盘上的脚本(逻辑路径;包内来源/非法路径 → false + 可读文本)。
		virtual bool ScriptsOpenExternal(const std::string& logicalPath, std::string* message = nullptr)
		{
			(void)logicalPath;
			if (message) *message = "scripts panel open is not wired to a host";
			return false;
		}
		// 从 scripts/templates/WorldScript.lua 复制出 scripts/script_<n>.lua(磁盘冲突递增、永不覆盖);
		// 成功时 outLogicalPath = 新脚本的逻辑路径,并把提示写进 message。
		virtual bool ScriptsCreateFromTemplate(std::string& outLogicalPath, std::string* message = nullptr)
		{
			(void)outLogicalPath;
			if (message) *message = "scripts panel create is not wired to a host";
			return false;
		}
		// ---- P1b D8a2:项目渲染设置(Project Settings 面板)----
		// 把设置写回 project.we.yaml。默认未接线 = false + 可读 message。
		// (面板即时预览走 World::RenderSettings::Set,不需要宿主。)
		virtual bool SaveProjectRenderSettings(const Asset::RenderingSettings& settings, std::string* message = nullptr)
		{
			(void)settings;
			if (message) *message = "settings panel is not wired to a host";
			return false;
		}
		// P4-1:物理设置(与渲染设置同一个面板/同一次保存)。
		virtual bool SaveProjectPhysicsSettings(const Asset::PhysicsSettingsData& settings, std::string* message = nullptr)
		{
			(void)settings;
			if (message) *message = "settings panel is not wired to a host";
			return false;
		}
		// P4-UX11:项目启动项(renderer / start_scene / content_root)—— 与渲染/物理同一份清单,
		// 只覆盖这三个字段(Load 后局部改,保留 id/包列表/注释)。
		virtual bool SaveProjectStartupSettings(const std::string& renderer, const std::string& startScene,
			const std::string& contentRoot, std::string* message = nullptr)
		{
			(void)renderer; (void)startScene; (void)contentRoot;
			if (message) *message = "settings panel is not wired to a host";
			return false;
		}
		// 内容根下的场景(逻辑路径,如 "scenes/3DTest.wd"),给"启动场景"下拉用。
		virtual std::vector<std::string> ListProjectScenes() { return {}; }
		// P4-UX11:发行包列表(project.we.yaml 的 `packages:`,打包产物里的相对路径)。
		virtual bool SaveProjectPackages(const std::vector<std::string>& packages, std::string* message = nullptr)
		{
			(void)packages;
			if (message) *message = "settings panel is not wired to a host";
			return false;
		}
		// 切换渲染后端 = 需要重启编辑器(运行中热切换会串资源):面板改完立刻生效。
		virtual void ApplyProjectRendererChange(const std::string& renderer) { (void)renderer; }
		// P4-U4:项目级"资产导入默认值"(`project.we.yaml` 的 `imports:` 区块)。
		// 实现口径:Locate → Load(保留其它字段与注释) → 只覆盖 ImportDefaults → Save,
		// 并把值写进 `ModelImportSettings::SetProjectDefaults` —— 之后新建导入立刻按新默认走
		// (已有产物的源不受影响:资产 meta 里的逐源设置优先)。
		virtual bool SaveProjectImportDefaults(const Asset::ModelImportSettings& settings,
			std::string* message = nullptr)
		{
			(void)settings;
			if (message) *message = "settings panel is not wired to a host";
			return false;
		}
		// ---- W9-2:内置脚本编辑器 ----
		// 打开脚本编辑器(逻辑路径;每个脚本一个 "script:<逻辑路径>" 面板)。默认空实现。
		// 实现必须是"创建后默认附加到主窗口"(用户 2026-09-18 决定)。
		virtual void OpenScriptEditor(const std::string& logicalPath) { (void)logicalPath; }
		// 关闭一个动态面板(脚本编辑器工具栏 Close 按钮)。默认空实现。
		virtual void CloseEditorPanel(const std::string& panelId) { (void)panelId; }
		// ---- W5-L1:文档场景的外部改动提示(EditorShell 转发 EditorLayer;默认未接线)----
		// 磁盘上的 .wd 内容变化 → 视口顶部提示条 + "重新打开"按钮;**不自动替换**文档。
		virtual bool ExternalSceneChanged() const { return false; }
		virtual void ReopenExternalScene() {}
		// ---- P1b D5:模型资产(.wmodel)实例化 ----
		// 内容浏览器双击 .wmodel → 把节点树实例化进当前文档场景(编辑态);
		// 默认未接线 = false + 可读 message。
		virtual bool InstantiateModelFile(const std::string& logicalPath, std::string* message = nullptr)
		{
			(void)logicalPath;
			if (message) *message = "model instantiation is not wired to a host";
			return false;
		}
		// 内容浏览器双击 .gltf/.glb → 导入(与 File 菜单同一条宿主路径);
		// outLogicalModel 非空时回传写出的 .wmodel 逻辑路径(供随后打开预览)。
		virtual bool ImportModelFile(const std::string& sourcePath, std::string* message = nullptr,
			std::string* outLogicalModel = nullptr)
		{
			(void)sourcePath;
			(void)outLogicalModel;
			if (message) *message = "glTF import is not wired to a host";
			return false;
		}
		// D10:带**目的地**的导入(逻辑目录,相对内容根;空 = 内核默认 = 源所在目录)。
		// 内容浏览器双击/拖放与"导入到当前文件夹"用这条;默认未接线 = 回退到上面那条。
		virtual bool ImportModelFileTo(const std::string& sourcePath, const std::string& destinationLogicalDir,
			std::string* message = nullptr, std::string* outLogicalModel = nullptr)
		{
			(void)destinationLogicalDir;
			return ImportModelFile(sourcePath, message, outLogicalModel);
		}
		// 打开模型预览(独立窗口,只读;不改场景)。默认未接线 = 无操作。
		virtual void OpenModelPreview(const std::string& logicalPath) { (void)logicalPath; }
		// D10(用户 2026-09-19):让**内容浏览器**打开"导入位置"选择器 —— 引擎内的树状选择,
		// 范围限定在内容根内(不用原生文件夹对话框,避免选到工作区外)。
		// sourcePath = 已选好的源文件;默认未接线 = 无操作(调用方应给出可读提示)。
		virtual void RequestImportDestination(const std::string& sourcePath) { (void)sourcePath; }
		// P4-UX16:面板向状态栏推一条短提示(新建资产失败等)。默认空实现 = 宿主未接线。
		// 不要用它做模态:失败信息是"说出来"而不是"挡住用户"。
		virtual void Notify(const std::string& message) { (void)message; }
		// P4-U6b:面板自绘的**帧级模态**(如属性面板的"添加组件"居中窗口)占用声明。
		// 传 panelId = 该面板正显示模态;传空 = 关闭。宿主据此在帧初封锁整窗输入,
		// 并在渲染该面板之前解开封锁(模态自己的控件才点得动),画完再封回去。
		virtual void SetPanelModalOwner(const std::string& panelId) { (void)panelId; }
	};

	// 编辑器面板组件:model 与 view 内聚,由 EditorShell 按停靠布局驱动渲染。
	class EditorPanel
	{
	public:
		virtual ~EditorPanel() = default;
		virtual const char* Id() const = 0;
		virtual const char* Title() const = 0;
		virtual void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) = 0;
		// W9-2:面板级快捷键(三层路由的第 2 层)。默认不消费,由宿主(EditorShell)按焦点
		// 面板分派;返回 true = 已消费,不再下探到引擎全局命令表。
		virtual bool OnShortcut(uint32_t keyCode, bool ctrl, bool shift, bool alt)
		{
			(void)keyCode;
			(void)ctrl;
			(void)shift;
			(void)alt;
			return false;
		}
	};
}
