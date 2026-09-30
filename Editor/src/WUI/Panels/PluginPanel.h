#pragma once

#include "EditorPanel.h"

#include <string>

namespace World
{
	class EditorShell;

	// PLUG-T3b:插件贡献的编辑器面板(每个注册面板一个实例,id = `plugin.panel.<pluginId>.<id>`)。
	//
	// 与插件管理器 / 项目设置同款:**独立窗口形态** —— 默认打开走
	// TogglePanel → OpenPanelAttached(附加到主窗口的标签切换),可拖出为独立 OS 窗口。
	// 内容由插件自己的 WeEditorPanelDesc::Draw 决定;本类只做"面板容器 → 宿主桥"转发
	// (渲染 + 命名空间 + 兜底:面板已被注销 = 画一行可读提示,不崩)。
	//
	// 生命周期由 EditorShell 持有;插件卸载时 PluginEditorHost 会请求关闭对应面板
	// (EditorShell::ClosePluginPanel),所以正常路径上不存在"注册表里有、宿主没了"的窗口。
	class PluginPanel final : public EditorPanel
	{
	public:
		PluginPanel(EditorShell& shell, std::string panelId);
		const char* Id() const override { return m_Id.c_str(); }
		// 标题是动态的(PanelTitle 统一从 PluginEditorHost 取),这里是兜底。
		const char* Title() const override { return m_Id.c_str(); }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		EditorShell& m_Shell;
		std::string m_Id;   // plugin.panel.<pluginId>.<id>(面板注册表 id / a11y 根 id)
	};
}
