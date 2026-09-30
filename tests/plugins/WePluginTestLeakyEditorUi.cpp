// PLUG-T3b 负例:注册了编辑器命令 + 面板,**故意不在 Unregister 里注销** ——
// 宿主必须在卸载 / Register false / Register 抛异常 / 管理器析构四条路径的兜底回收里
// 把命令与面板从编辑器注册表移除(记警告),不留指向已卸载 DLL 的回调。
#include "PluginTestSupport.h"

#include <cstddef>
#include <string>

using namespace World::Plugins;

namespace
{
	constexpr const char* kCommandId = "test.leakyeditorui.ping";
	constexpr const char* kPanelId = "test.leakyeditorui.panel";

	void OnCommand(void*)
	{
		WeTestPlugin::Log("leaky editor command invoked");
	}

	int DrawPanel(void*, const WeEditorUiApi* ui, void* uiContext)
	{
		if (ui && uiContext && ui->Label)
			ui->Label(uiContext, "leaky editor panel");
		return 0;
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host)
			|| host.StructSize < offsetof(WeHostApi, UnregisterEditorPanel)
				+ sizeof(host.UnregisterEditorPanel)
			|| !host.RegisterEditorCommand || !host.RegisterEditorPanel)
		{
			WeTestPlugin::Log("host editor surface missing; rejected", WePluginLogError);
			return false;
		}
		WeTestPlugin::Host() = host;

		WeEditorCommandDesc command;
		command.Id = kCommandId;
		command.Label = "Leaky Editor Command";
		command.Callback = &OnCommand;
		const bool commandOk = host.RegisterEditorCommand(host.UserData, &command);

		WeEditorPanelDesc panel;
		panel.Id = kPanelId;
		panel.Title = "Leaky Editor Panel";
		panel.Draw = &DrawPanel;
		const bool panelOk = host.RegisterEditorPanel(host.UserData, &panel);

		WeTestPlugin::Log((commandOk && panelOk) ? "registered (editor extensions will leak)"
			: "leaky editor registration failed",
			(commandOk && panelOk) ? WePluginLogInfo : WePluginLogError);
		return commandOk && panelOk;
	}

	void Unregister(World::WorldContext&)
	{
		// 故意不注销:验证宿主卸载兜底(警告 + 从编辑器注册表移除)。
		WeTestPlugin::Log("unregistered (editor extensions intentionally left behind)");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.leakyeditorui";
		plugin.Name = "Test Leaky Editor UI";
		plugin.Version = "1.0.0";
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
