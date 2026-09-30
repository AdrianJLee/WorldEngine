#pragma once

#include "EditorPanel.h"

#include <string>

namespace World
{
	class EditorShell;

	// PLUG-T3(用户 2026-09-30「要有个插件管理器」):插件管理器面板。
	//
	// 列表 = `PluginManager::Entries()`(id / 名称 / 来源 / 版本 / ABI / 状态 / 顺序 / 根路径);
	// 选中详情 = 状态 / 诊断 / 依赖 / 提供的能力 / 导出 / 根路径;
	// 操作 = 在内容浏览器中定位(仅项目插件)/ 复制诊断 / 启用-禁用(仅引擎插件,写
	// `local/plugins.json`,下次启动生效)。
	//
	// 形态:停靠面板,**只在项目形态注册**(E2:启动器形态不注册面板、Window 菜单不出现;
	// 无项目时 `plugin.*` AI 命令返回"需要先打开项目")。
	//
	// 数据与动作都走 EditorShell(PluginManager 由 EditorLayer 在挂载项目后持有并 Discover/Load;
	// 面板不直接抓 EditorLayer,也不自建第二份插件状态)。
	class PluginsPanel final : public EditorPanel
	{
	public:
		explicit PluginsPanel(EditorShell& shell);
		const char* Id() const override { return "plugins"; }
		const char* Title() const override { return "Plugins"; }
		void OnRender(Wui::WuiContext& ctx, const Wui::WuiRect& rect, PanelHost& host) override;

	private:
		EditorShell& m_Shell;
		// 选中插件的 id(空 = 尚未选过;渲染时自动选中第一行,不写回成员以外的地方)。
		std::string m_SelectedId;
	};
}
