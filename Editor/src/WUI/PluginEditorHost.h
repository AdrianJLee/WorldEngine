#pragma once

#include "World/Plugins/PluginHostServices.h"
#include "World/Plugins/PluginManager.h"
#include "World/Plugins/WePluginApi.h"
#include "World/WUI/WuiCore.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace World
{
	class EditorShell;

	// PLUG-T3b:插件编辑器扩展面在**编辑器侧**的落点 —— 实现 Engine 声明的
	// World::Plugins::PluginEditorHost,把插件注册的命令 / 面板真正接进编辑器。
	//
	// 数据分工(单一事实源):
	//   * PluginManager = 账本(谁注册了什么 + 卸载/回滚/析构四条路径的兜底回收);
	//   * PluginEditorHost(本类)= 编辑器侧的注册表(命令名 → 插件回调;面板 id → 控制器)
	//     与渲染桥(面板 Draw 的 WUI 小组件表 + 只读上下文句柄)。
	// 本类**不拥有** PluginManager;由 EditorLayer 在插件加载前 SetEditorHost 注入。
	//
	// 命名契约(与方案 §14 T3b 一致,探针据此断言):
	//   * 面板注册表 id / a11y 根 id = `plugin.panel.<pluginId>.<面板 id>`;
	//   * 插件命令在宿主命令面里的名字 = `plugin.command.<pluginId>.<命令 id>`
	//     (AI 通道 `plugin.command.run <名字>` 触发同一条执行路径);
	//   * 面板里的控件 a11y id = `plugin.panel.<pluginId>.<控件 id>`(在 Draw 上下文里命名空间化,
	//     插件只需给面板内唯一的短 id)。
	class PluginEditorHost final : public Plugins::PluginEditorHost
	{
	public:
		explicit PluginEditorHost(EditorShell& shell);

		// ---- Plugins::PluginEditorHost ----
		bool RegisterEditorCommand(const std::string& pluginId, const Plugins::WeEditorCommandDesc& desc,
			Plugins::WeEditorCommandCallback callback, void* userData) override;
		bool UnregisterEditorCommand(const std::string& pluginId, const std::string& id) override;
		bool RegisterEditorPanel(const std::string& pluginId, const Plugins::WeEditorPanelDesc& desc,
			Plugins::PluginEditorPanelRenderFn render, void* userData) override;
		bool UnregisterEditorPanel(const std::string& pluginId, const std::string& id) override;
		int RenderEditorPanelSurface(const Plugins::PluginEditorPanel& panel,
			Plugins::WeEditorPanelDrawFn draw, void* userData) override;

		// ---- 编辑器侧查询(EditorShell / AI 通道用;只读)----
		// 面板标题(注册表没有该面板 = 空串,宿主据此回退到面板 id)。
		std::string PanelTitle(const std::string& panelId) const;
		// 已注册的插件面板 id(确定性顺序)与是否注册。
		std::vector<std::string> PanelIds() const;
		bool HasPanel(const std::string& panelId) const { return m_Panels.count(panelId) > 0; }
		// 触发一条插件命令(名字 = plugin.command.<pluginId>.<id>;未注册 = false + 原因)。
		bool InvokeEditorCommand(const std::string& commandName, std::string* error = nullptr);

	private:
		struct Command
		{
			std::string PluginId;
			std::string RawId;
			Plugins::WeEditorCommandCallback Callback = nullptr;
			void* UserData = nullptr;
		};
		struct Panel
		{
			std::string PluginId;
			std::string RawId;
			std::string DisplayId;
			Plugins::PluginEditorPanelRenderFn Render = nullptr;
			void* UserData = nullptr;
		};

		EditorShell& m_Shell;
		std::unordered_map<std::string, Command> m_Commands;   // key = plugin.command.<pluginId>.<id>
		std::unordered_map<std::string, Panel> m_Panels;       // key = plugin.panel.<pluginId>.<id>
	};
}
