#pragma once

#include "EditorPanel.h"

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
		Entity m_Context;
		glm::vec2 m_MenuPos {};
		glm::vec2 m_BlankMenuPos {};
	};
}
