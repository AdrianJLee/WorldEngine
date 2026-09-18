#pragma once

#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiWidgets.h"
#include "World/WUI/WuiGizmo.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

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
	};

	// 编辑器面板组件:model 与 view 内聚,由 EditorShell 按停靠布局驱动渲染。
	class EditorPanel
	{
	public:
		virtual ~EditorPanel() = default;
		virtual const char* Id() const = 0;
		virtual const char* Title() const = 0;
		virtual void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) = 0;
	};
}
