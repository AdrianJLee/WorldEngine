// PLUG-T3b 正例:编辑器扩展注册(WeHostApi::RegisterEditorCommand / RegisterEditorPanel)。
//
// 覆盖(插件侧证据全部经 host.Log 回传,由 World.Plugins 单测取回):
//   * 命令注册成功 + 同 id 第二次被拒(不覆盖)+ 坏描述干净拒绝(StructSize 不符 / 空 id / 无回调);
//   * 面板注册成功 + 同 id 第二次被拒;
//   * 导出 `editorui.unregister`(单测经 LookupExport 调用)—— 按需注销命令 / 面板,
//     让单测观察到"注销后宿主注册表立刻为空 + 再次注销幂等";
//   * 命令回调计数可观测(单测经 EditorCommands() 的 InvokeCount 断言);
//   * 面板 Draw 走宿主小组件表:Label / Separator / Checkbox / Button(按钮经
//     WeEditorUiApi::InvokeCommand 触发本插件的命令 —— 与 AI 通道同一条宿主命令路径),
//     `ui.InvokeCommand` 失败的路径也留一条可读日志(便于排查)。
// 只依赖公共 ABI 头(不链接 World)。
#include "PluginTestSupport.h"

#include <cstddef>
#include <cstdint>
#include <string>

using namespace World::Plugins;

namespace
{
	constexpr const char* kCommandId = "test.editorui.ping";
	constexpr const char* kPanelId = "test.editorui.panel";
	constexpr const char* kSecondCommandId = "test.editorui.second";

	uint32_t g_CommandCount = 0;

	bool SurfaceAvailable(const WeHostApi& host)
	{
		return host.StructSize >= offsetof(WeHostApi, UnregisterEditorPanel)
				+ sizeof(host.UnregisterEditorPanel)
			&& host.RegisterEditorCommand != nullptr && host.UnregisterEditorCommand != nullptr
			&& host.RegisterEditorPanel != nullptr && host.UnregisterEditorPanel != nullptr;
	}

	void OnPing(void* userData)
	{
		++g_CommandCount;
		const std::string message = std::string(static_cast<const char*>(userData) ? static_cast<const char*>(userData)
			: "command") + ": plugin command invoked count=" + std::to_string(g_CommandCount);
		WeTestPlugin::Log(message.c_str());
	}

	int DrawPanel(void* userData, const WeEditorUiApi* ui, void* uiContext)
	{
		if (!ui || !uiContext || !ui->Label || !ui->Separator || !ui->Checkbox || !ui->Button)
		{
			WeTestPlugin::Log("panel draw: UI surface missing", WePluginLogError);
			return -1;
		}
		const char* const context = static_cast<const char*>(userData);
		ui->Label(uiContext, context ? context : "editor-ui test panel");
		ui->Separator(uiContext);
		static bool enabled = true;
		ui->Checkbox(uiContext, "enabled", "Enabled", &enabled);
		if (ui->Button(uiContext, "ping", "Ping"))
		{
			if (ui->InvokeCommand && ui->InvokeCommand(uiContext, "test.editorui.ping"))
				WeTestPlugin::Log("panel button invoked the command");
			else
				WeTestPlugin::Log("panel button could not invoke the command", WePluginLogWarn);
		}
		return 0;
	}

	bool RegisterBadDescriptors(const WeHostApi& host)
	{
		// ① StructSize 小于头文件声明的结构(旧头) —— 干净拒绝。
		WeEditorCommandDesc smallSize;
		smallSize.StructSize = sizeof(WeEditorCommandDesc) - 4;
		smallSize.Id = "test.editorui.badsize";
		smallSize.Callback = &OnPing;
		const bool sizeOk = host.RegisterEditorCommand(host.UserData, &smallSize);
		WeTestPlugin::Log(sizeOk ? "editor-size-accepted" : "editor-size-rejected",
			sizeOk ? WePluginLogError : WePluginLogInfo);

		// ② 空 id —— 干净拒绝。
		WeEditorCommandDesc emptyId;
		emptyId.Id = "";
		emptyId.Callback = &OnPing;
		const bool emptyOk = host.RegisterEditorCommand(host.UserData, &emptyId);
		WeTestPlugin::Log(emptyOk ? "editor-empty-accepted" : "editor-empty-rejected",
			emptyOk ? WePluginLogError : WePluginLogInfo);

		// ③ 没有回调 —— 干净拒绝。
		WeEditorCommandDesc noCallback;
		noCallback.Id = "test.editorui.nocallback";
		const bool callbackOk = host.RegisterEditorCommand(host.UserData, &noCallback);
		WeTestPlugin::Log(callbackOk ? "editor-nocallback-accepted" : "editor-nocallback-rejected",
			callbackOk ? WePluginLogError : WePluginLogInfo);

		return !sizeOk && !emptyOk && !callbackOk;
	}

	bool UnregisterById(const char* rawId)
	{
		const WeHostApi& host = WeTestPlugin::Host();
		if (!rawId || !host.UserData || !host.UnregisterEditorCommand || !host.UnregisterEditorPanel)
			return false;
		std::string id = rawId;
		const bool isPanel = id.rfind("panel:", 0) == 0;
		if (isPanel)
			id = id.substr(6);
		const bool first = isPanel ? host.UnregisterEditorPanel(host.UserData, id.c_str())
			: host.UnregisterEditorCommand(host.UserData, id.c_str());
		// 幂等:第二次同样返回 true。
		const bool second = isPanel ? host.UnregisterEditorPanel(host.UserData, id.c_str())
			: host.UnregisterEditorCommand(host.UserData, id.c_str());
		const std::string message = std::string("editorui.unregister ") + (isPanel ? "panel " : "command ")
			+ id + " -> " + ((first && second) ? "idempotent-ok" : "failed");
		WeTestPlugin::Log(message.c_str(), (first && second) ? WePluginLogInfo : WePluginLogError);
		return first && second;
	}

	const WePluginExport kExports[] =
	{
		{ "editorui.unregister", 1, reinterpret_cast<void*>(&UnregisterById) },
	};

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host) || !SurfaceAvailable(host))
		{
			WeTestPlugin::Log("host editor surface missing; rejected", WePluginLogError);
			return false;
		}
		WeTestPlugin::Host() = host;

		if (!RegisterBadDescriptors(host))
		{
			WeTestPlugin::Log("bad editor descriptors were not rejected", WePluginLogError);
			return false;
		}

		WeEditorCommandDesc command;
		command.Id = kCommandId;
		command.Label = "Editor UI Test Ping";
		command.Tooltip = "PLUG-T3b test command";
		command.Callback = &OnPing;
		command.UserData = const_cast<char*>("command");
		const bool commandOk = host.RegisterEditorCommand(host.UserData, &command);
		const bool duplicate = host.RegisterEditorCommand(host.UserData, &command);
		WeTestPlugin::Log(commandOk ? "editor-command-registered" : "editor-command-register-failed",
			commandOk ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log(duplicate ? "editor-command-duplicate-accepted"
			: "editor-command-duplicate-rejected", WePluginLogInfo);

		WeEditorCommandDesc second;
		second.Id = kSecondCommandId;
		second.Label = "Editor UI Test Second";
		second.Callback = &OnPing;
		const bool secondOk = host.RegisterEditorCommand(host.UserData, &second);
		WeTestPlugin::Log(secondOk ? "editor-second-registered" : "editor-second-register-failed",
			secondOk ? WePluginLogInfo : WePluginLogError);

		WeEditorPanelDesc panel;
		panel.Id = kPanelId;
		panel.Title = "Editor UI Test Panel";
		panel.Draw = &DrawPanel;
		panel.UserData = const_cast<char*>("PLUG-T3b editor panel");
		const bool panelOk = host.RegisterEditorPanel(host.UserData, &panel);
		const bool duplicatePanel = host.RegisterEditorPanel(host.UserData, &panel);
		WeTestPlugin::Log(panelOk ? "editor-panel-registered" : "editor-panel-register-failed",
			panelOk ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log(duplicatePanel ? "editor-panel-duplicate-accepted"
			: "editor-panel-duplicate-rejected", WePluginLogInfo);

		if (!commandOk || !secondOk || !panelOk)
			return false;
		WeTestPlugin::Log("editor-registered");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		const WeHostApi& host = WeTestPlugin::Host();
		if (!host.UserData || !host.UnregisterEditorCommand || !host.UnregisterEditorPanel)
		{
			WeTestPlugin::Log("editor unregister surface missing", WePluginLogError);
			return;
		}
		// 按契约成对注销(即使已经被导出回调注销过 —— 幂等)。
		const bool commandFirst = host.UnregisterEditorCommand(host.UserData, kCommandId);
		const bool commandSecond = host.UnregisterEditorCommand(host.UserData, kCommandId);
		const bool secondCommand = host.UnregisterEditorCommand(host.UserData, kSecondCommandId);
		const bool panelFirst = host.UnregisterEditorPanel(host.UserData, kPanelId);
		const bool panelSecond = host.UnregisterEditorPanel(host.UserData, kPanelId);
		const bool ok = commandFirst && commandSecond && secondCommand && panelFirst && panelSecond;
		WeTestPlugin::Log(ok ? "editor-unregistered idempotent" : "editor-unregister-failed",
			ok ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log("editor-unregistered");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.editorui";
		plugin.Name = "Test Editor UI";
		plugin.Version = "1.0.0";
		plugin.Exports = kExports;
		plugin.ExportCount = 1;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
