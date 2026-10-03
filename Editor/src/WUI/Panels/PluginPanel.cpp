#include "wldpch.h"
#include "WUI/Panels/PluginPanel.h"

#include "WUI/Shell/EditorShell.h"

namespace World
{
	PluginPanel::PluginPanel(EditorShell& shell, std::string panelId)
		: m_Shell(shell), m_Id(std::move(panelId)) {}

	void PluginPanel::OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host)
	{
		(void)host;
		// 容器只做转发:实际绘制走编辑器宿主(PluginEditorHost → PluginManager → 插件的 Draw)。
		// 面板已被注销(插件卸载/禁用)时宿主画一行可读空态 —— 不静默画旧内容。
		m_Shell.DrawPluginPanel(ctx, rect, m_Id);
	}
}
