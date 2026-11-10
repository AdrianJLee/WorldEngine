// PLUG-T1 负例:Register 返回 false(契约 = 插件自己已清干净)→ 宿主不调用 Unregister。
#include "PluginTestSupport.h"

using namespace World::Plugins;

namespace
{
	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host))
			return false;
		WeTestPlugin::Host() = host;
		WeTestPlugin::Log("register-attempted");
		return false;
	}

	void Unregister(World::WorldContext&)
	{
		WeTestPlugin::Log("unregister-called");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.fail";
		plugin.Name = "Test Registration Failure";
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
