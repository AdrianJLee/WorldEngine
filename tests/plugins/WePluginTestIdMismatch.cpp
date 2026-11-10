// PLUG-T1 负例:WePlugin.Id 与清单 id 不一致 → 拒绝对不上号的插件(防"挂别人的名")。
#include "PluginTestSupport.h"

using namespace World::Plugins;

namespace
{
	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		WeTestPlugin::Host() = host;
		WeTestPlugin::Log("registered");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		WeTestPlugin::Log("unregistered");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.other";
		plugin.Name = "Test Id Mismatch";
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
