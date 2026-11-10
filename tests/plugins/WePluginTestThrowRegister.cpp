// PLUG-T1 负例:Register 抛异常(契约违约,可能留半注册)→ 宿主 best-effort Unregister 回滚。
#include "PluginTestSupport.h"

#include <stdexcept>

using namespace World::Plugins;

namespace
{
	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host))
			return false;
		WeTestPlugin::Host() = host;
		WeTestPlugin::Log("register-throw");
		throw std::runtime_error("test plugin registration failure");
	}

	void Unregister(World::WorldContext&)
	{
		WeTestPlugin::Log("unregister-after-throw");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.throw";
		plugin.Name = "Test Registration Throw";
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
