// PLUG-T1 负例:Register 为空 → 拒绝(插件必须提供注册入口)。
#include "PluginTestSupport.h"

using namespace World::Plugins;

namespace
{
	void Unregister(World::WorldContext&)
	{
		WeTestPlugin::Log("unregistered");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.noregister";
		plugin.Name = "Test Without Register";
		plugin.Register = nullptr;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
