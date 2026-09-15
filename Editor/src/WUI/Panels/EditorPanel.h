#pragma once

#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

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
