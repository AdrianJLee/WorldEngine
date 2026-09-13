#pragma once

#include "ViewportHost.h"
#include "World/Scene/Components.h"

namespace World
{
	// 视口面板:场景画面、拾取、Gizmo 与播放控制。Gizmo 拖拽前后快照仅用于标脏。
	class ViewportPanel final : public EditorPanel
	{
	public:
		explicit ViewportPanel(ViewportHost& host) : m_Host(host) {}
		const char* Id() const override { return "view"; }
		const char* Title() const override { return "View"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		ViewportHost& m_Host;
		bool m_GizmoActive = false;
		TransformComponent m_GizmoBefore;
	};
}
