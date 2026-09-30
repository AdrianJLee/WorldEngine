// PLUG-T3b 负例:注册了编辑器命令 + 面板之后**抛异常**(Register 契约违约)。
// 宿主 best-effort 调 Unregister 回滚再回收:命令与面板必须从编辑器注册表移除,
// 且不留半注册(与 T1 的 throw 负例同一口径,只是这次带上了编辑器扩展面)。
#include "PluginTestSupport.h"

#include <cstddef>
#include <stdexcept>
#include <string>

using namespace World::Plugins;

namespace
{
	constexpr const char* kCommandId = "test.throwingeditorui.ping";
	constexpr const char* kPanelId = "test.throwingeditorui.panel";

	void OnCommand(void*)
	{
		WeTestPlugin::Log("throwing editor command invoked");
	}

	int DrawPanel(void*, const WeEditorUiApi* ui, void* uiContext)
	{
		if (ui && uiContext && ui->Label)
			ui->Label(uiContext, "throwing editor panel");
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
		command.Label = "Throwing Editor Command";
		command.Callback = &OnCommand;
		host.RegisterEditorCommand(host.UserData, &command);

		WeEditorPanelDesc panel;
		panel.Id = kPanelId;
		panel.Title = "Throwing Editor Panel";
		panel.Draw = &DrawPanel;
		host.RegisterEditorPanel(host.UserData, &panel);

		WeTestPlugin::Log("registered then throwing");
		throw std::runtime_error("PLUG-T3b editor registration failure");
	}

	void Unregister(World::WorldContext&)
	{
		// 宿主在异常路径 best-effort 调这里(插件自己不做清理 —— 由兜底回收兜住)。
		WeTestPlugin::Log("unregister-after-editor-throw");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.throwingeditorui";
		plugin.Name = "Test Throwing Editor UI";
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
