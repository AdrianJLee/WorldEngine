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
