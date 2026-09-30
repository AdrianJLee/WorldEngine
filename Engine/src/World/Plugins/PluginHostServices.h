#pragma once

// ============================================================================
// PLUG-T3b:插件编辑器扩展面(命令 / 面板)的**宿主服务接口**。
//
// 为什么单独一份接口:插件 ABI(WePluginApi.h)只含 C 类型,必须能跨 DLL 边界;
// 而"命令进编辑器命令面、面板进编辑器面板注册表"是宿主侧(Editor)的能力。
// PluginManager 只做**账本 + 回收**(谁注册了什么、卸载时兜底),实际接进编辑器 UI 的
// 工作交给实现了本接口的宿主(Editor/src/Plugins/PluginEditorHost.h)。
//
// 纪律:
//   * 指针所有权归宿主;PluginManager 只保存非拥有指针(SetEditorHost 设置,nullptr = 未接线)。
//   * 注册失败 = 返回 false + 由实现写可读诊断(PluginManager 侧还会再记一条 WARN);
//     注册成功 = 实现必须记住该条,**Unregister* 必须能被独立调用**(卸载/回滚/析构兜底)。
//   * 卸载顺序不变:宿主先 Unregister → PluginManager 兜底 Reclaim → 释放 DLL;
//     因此实现在 Unregister 之后**不得**再触摸 desc/callback 指向的插件内存。
// ============================================================================

#include "World/Plugins/WePluginApi.h"

#include <cstdint>
#include <string>

namespace World::Plugins
{
	// 编辑器命令的一条快照:PluginManager/编辑器命令面/面板管理层共用的可观测结构
	// (AI 命令 `editor.commands` 与探针断言的数据源)。
	struct PluginEditorCommand
	{
		std::string PluginId;      // 拥有者插件 id(清单 id)
		std::string Id;            // 插件自己的命令 id(RegisterEditorCommand 的 desc.Id)
		std::string Label;         // 显示名(可空 = Id)
		std::string Tooltip;       // 悬停提示(可空)
		std::string CommandName;   // 宿主命令面里的完整名字:plugin.command.<pluginId>.<id>
		uint64_t InvokeCount = 0;  // 成功执行次数(宿主统计;探针据此断言"命令被触发过")
	};

	// 编辑器面板的一条快照:面板管理层据此建实例 / 解析标题 / 渲染。
	struct PluginEditorPanel
	{
		std::string PluginId;
		std::string Id;            // 插件自己的面板 id
		std::string DisplayId;     // 面板注册表 id / a11y 根 id:plugin.panel.<pluginId>.<id>
		std::string Title;         // 窗口/标签标题(可空 = Id)
	};

	// 插件面板绘制入口(宿主实现;PluginManager::RenderEditorPanel 调用它执行 Draw)。
	// 返回 0 = 正常;非 0 = 面板缺失 / 宿主未接线 / 插件 Draw 违约(异常 / 非零返回)。
	using PluginEditorPanelRenderFn = int (*)(void* host,
		const PluginEditorPanel& panel, WeEditorUiApi* ui, void* uiContext, void* userData);

	// 编辑器侧插件扩展面的宿主接口(Editor 实现;PluginManager 只保存非拥有指针)。
	class PluginEditorHost
	{
	public:
		virtual ~PluginEditorHost() = default;

		// 命令:注册进编辑器命令面(可从 AI 通道触发 / 由插件面板按钮触发)。
		// 重复名字 = false(不覆盖,调用方给可读日志)。
		virtual bool RegisterEditorCommand(const std::string& pluginId,
			const WeEditorCommandDesc& desc, WeEditorCommandCallback callback, void* userData) = 0;
		// 幂等:注销本插件没注册过的命令 = true。
		virtual bool UnregisterEditorCommand(const std::string& pluginId, const std::string& id) = 0;

		// 面板:注册进编辑器面板注册表(独立窗口形态,与插件管理器同款 —— 默认附加到
		// 主窗口的标签切换,可拖出为独立 OS 窗口)。重复 DisplayId = false。
		virtual bool RegisterEditorPanel(const std::string& pluginId,
			const WeEditorPanelDesc& desc, PluginEditorPanelRenderFn render, void* userData) = 0;
		// 幂等:注销本插件没注册过的面板 = true(它可能已经被宿主关掉/清理)。
		virtual bool UnregisterEditorPanel(const std::string& pluginId, const std::string& id) = 0;

		// 执行一个已注册面板的绘制(宿主面板层渲染时调用;panel = 管理器给的快照,
		// draw = 插件回调,userData = 插件注册时给的句柄)。返回 0 = 正常。
		virtual int RenderEditorPanelSurface(const PluginEditorPanel& panel,
			WeEditorPanelDrawFn draw, void* userData) = 0;
	};
}
