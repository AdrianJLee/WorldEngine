// {{PluginName}} — 编辑器扩展插件(Editor Extension Plugin 模板,{{PluginScope}} 形态)。
//
// T3b(2026-09-30)起是**真实注册**:Register 里经 WeHostApi::RegisterEditorCommand /
// RegisterEditorPanel 把一条命令与一个面板交给宿主;面板是**独立窗口形态**(默认附加到
// 主窗口的标签,可拖出为独立 OS 窗口),内容由本文件的 DrawPanel 用宿主给的**小组件表**
// (Label / Button / Separator / Checkbox)绘制;面板里的按钮经
// WeEditorUiApi::InvokeCommand 触发本插件注册的命令 —— 与 AI 通道
// (`plugin.command.run`)走同一条宿主命令路径。
//
// 命令与面板的 a11y id 由宿主按面板命名空间隔离:`plugin.panel.<插件 id>.<面板 id>` /
// `plugin.command.<插件 id>.<命令 id>`;本文件里的控件 id 只需面板内唯一。
//
// 只依赖公共 ABI 头(World/Plugins/WePluginApi.h),不链接 World。
#include "World/Plugins/WePluginApi.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#  define WE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define WE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
	using namespace World::Plugins;

	// 必须与 plugin.we.yaml 的 id 一致(宿主以清单为准;不一致 = 拒绝加载)。
	constexpr const char* kPluginId = "{{PluginId}}";
	constexpr const char* kPluginName = "{{PluginName}}";

	// Unregister 无宿主表参数(ABI 契约)⇒ 插件自己记住 Register 时拿到的那张表。
	WeHostApi& Host()
	{
		static WeHostApi host;
		return host;
	}

	// 面板自己的小状态:命令执行次数(按钮点击时 +1,面板上给它一行可读回显)。
	uint32_t g_CommandCount = 0;
	bool g_ShowStatusLine = true;

	void OnHello(void* userData)
	{
		++g_CommandCount;
		const WeHostApi& host = Host();
		if (host.Log && host.UserData)
			host.Log(host.UserData, WePluginLogInfo, "{{PluginDir}}.hello invoked");
		(void)userData;
	}

	// 面板绘制:宿主每帧调用;uiContext = 本面板的只读上下文句柄(只回传给小组件表)。
	int DrawPanel(void* userData, const WeEditorUiApi* ui, void* uiContext)
	{
		(void)userData;
		if (!ui || !uiContext || !ui->Label || !ui->Button || !ui->Separator || !ui->Checkbox)
			return 1;   // 宿主缺小组件表 = 契约不满足(不静默画半截面板)

		ui->Label(uiContext, "{{PluginName}} — editor extension");
		ui->Separator(uiContext);
		ui->Checkbox(uiContext, "status-line", "Show status line", &g_ShowStatusLine);
		if (g_ShowStatusLine)
		{
			static char status[96];
			// 首期小组件表没有文本格式化 ⇒ 用 C 运行时的 snprintf 组一段自己的回显文本。
			std::snprintf(status, sizeof(status), "{{PluginDir}}.hello ran %u time(s)", g_CommandCount);
			ui->Label(uiContext, status);
		}
		if (ui->Button(uiContext, "hello", "Run {{PluginDir}}.hello"))
		{
			// 与 AI 通道 `plugin.command.run` 同一条宿主命令路径(执行计数在宿主侧可观测)。
			if (!ui->InvokeCommand || !ui->InvokeCommand(uiContext, "{{PluginDir}}.hello"))
			{
				const WeHostApi& host = Host();
				if (host.Log && host.UserData)
					host.Log(host.UserData, WePluginLogWarn,
						"panel button could not invoke {{PluginDir}}.hello");
			}
		}
		return 0;
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		// 双向校验:宿主表必须覆盖到编辑器扩展注册面(尾部追加字段的 offsetof + sizeof 判据)。
		if (host.AbiVersion != WE_PLUGIN_ABI_VERSION
			|| host.StructSize < offsetof(WeHostApi, UnregisterEditorPanel)
				+ sizeof(host.UnregisterEditorPanel)
			|| !host.Log || !host.RegisterEditorCommand || !host.UnregisterEditorCommand
			|| !host.RegisterEditorPanel || !host.UnregisterEditorPanel)
		{
			return false;   // 宿主太旧 / 缺能力 = 干净拒绝,不半注册
		}
		Host() = host;

		WeEditorCommandDesc command;
		command.Id = "{{PluginDir}}.hello";
		command.Label = "{{PluginName}}: Hello";
		command.Tooltip = "Runs the {{PluginDir}}.hello plugin command.";
		command.Callback = &OnHello;
		if (!host.RegisterEditorCommand(host.UserData, &command))
		{
			host.Log(host.UserData, WePluginLogError, "editor command registration failed");
			return false;
		}

		WeEditorPanelDesc panel;
		panel.Id = "{{PluginDir}}.panel";
		panel.Title = "{{PluginName}}";
		panel.Draw = &DrawPanel;
		if (!host.RegisterEditorPanel(host.UserData, &panel))
		{
			host.UnregisterEditorCommand(host.UserData, command.Id);   // 失败路径自己清干净
			host.Log(host.UserData, WePluginLogError, "editor panel registration failed");
			return false;
		}
		host.Log(host.UserData, WePluginLogInfo, "registered editor command + panel");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		const WeHostApi& host = Host();
		if (!host.UserData || !host.UnregisterEditorCommand || !host.UnregisterEditorPanel)
			return;
		host.UnregisterEditorPanel(host.UserData, "{{PluginDir}}.panel");       // 与 Register 成对
		host.UnregisterEditorCommand(host.UserData, "{{PluginDir}}.hello");
	}

	const char* const kProvides[] = { "editor.panel", "editor.command" };

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = kPluginId;
		plugin.Name = kPluginName;
		plugin.Version = "1.0.0";
		plugin.MinEngineVersion = ">=2.0";
		plugin.Provides = kProvides;
		plugin.ProvidesCount = 2;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

// 入口:宿主 ABI 不等值 = 干净拒绝(不按旧布局解释)。
WE_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	if (hostAbiVersion != WE_PLUGIN_ABI_VERSION)
		return nullptr;
	static const WePlugin plugin = MakePlugin();
	return &plugin;
}
