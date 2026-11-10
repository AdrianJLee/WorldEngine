// PLUG-T1 正常插件 B:依赖关系只写在清单里(不写死在 DLL),用于断言加载顺序 = 依赖拓扑。
#include "PluginTestSupport.h"

using namespace World::Plugins;

namespace
{
	const char* const kProvides[] = { "asset.type" };

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host))
			return false;
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
		plugin.Id = "test.beta";
		plugin.Name = "Test Beta";
		plugin.Version = "2.1.0";
		plugin.Provides = kProvides;
		plugin.ProvidesCount = 1;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
