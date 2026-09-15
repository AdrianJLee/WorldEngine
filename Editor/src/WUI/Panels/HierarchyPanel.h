#pragma once

#include "EditorPanel.h"
#include "World/WUI/WuiWidget.h"

#include <memory>

namespace World
{
	// 场景层级面板:实体列表、选择与上下文菜单。菜单目标跨帧保留在模型里。
	class HierarchyPanel final : public EditorPanel
	{
	public:
		const char* Id() const override { return "hierarchy"; }
		const char* Title() const override { return "Scene Hierarchy"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::shared_ptr<Wui::WuiScrollArea> m_Scroll;
		std::vector<std::shared_ptr<Wui::WuiListRow>> m_Rows;
		std::vector<Entity> m_RowEntities;
		std::string m_LastOrderKey;
		Entity m_Context;
		// 拖拽设父的待提交落点(拖拽结束后统一提交)。
		Entity m_PendingDropHandle;
		uint32_t m_PendingDropSource = 0;
		// 0=插到目标之前 1=成为目标子节点 2=插到目标之后
		uint32_t m_PendingDropZone = 1;
		glm::vec2 m_MenuPos {};
		glm::vec2 m_BlankMenuPos {};
	};
}
