#pragma once

#include "ViewportHost.h"
#include "World/Scene/Components.h"
#include "World/WUI/WuiWidget.h"

#include <memory>

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
		std::shared_ptr<Wui::WuiBox> m_Root;
		std::shared_ptr<Wui::WuiImage> m_SceneImage;
		// P4-U8a:悬浮的运行控制药丸(独立小布局树,按绝对矩形摆在画面之上,不占布局)。
		std::shared_ptr<Wui::WuiBox> m_ToolPill;
		std::vector<std::shared_ptr<Wui::WuiImageButton>> m_Tools;
		bool m_GizmoActive = false;
		TransformComponent m_GizmoBefore;
	};
}
