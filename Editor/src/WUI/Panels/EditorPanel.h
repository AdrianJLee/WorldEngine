#pragma once

#include "World/WUI/WuiContext.h"
#include "World/WUI/WuiWidgets.h"
#include "World/Scene/Entity.h"
#include "World/Scene/Scene.h"

namespace World
{
	class EditorLayer;

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
