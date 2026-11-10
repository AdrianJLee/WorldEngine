// PLUG-T1 负例:插件自报的 AbiVersion 与宿主不等值(StructSize 正确)→ 必须被干净拒绝。
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
		plugin.Id = "test.abi";
		plugin.Name = "Test ABI Mismatch";
		plugin.AbiVersion = WE_PLUGIN_ABI_VERSION + 1;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
