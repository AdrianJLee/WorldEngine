// PLUG-T2a:插件间 LookupExport —— 本插件依赖 test.alpha(其 Exports 有 alpha.ping v1),
// 在 Register 里经 WeHostApi::LookupExport 查命中/版本不足/名字不存在/插件不存在四种情况。
#include "PluginTestSupport.h"

#include <cstdint>

using namespace World::Plugins;

namespace
{
	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host) || !WeTestPlugin::HostApiHasRegistrationSurface(host))
		{
			WeTestPlugin::Log("host registration surface missing; rejected", WePluginLogError);
			return false;
		}
		WeTestPlugin::Host() = host;   // WeTestPlugin::Log 走这张表回传插件侧证据

		void* hit = host.LookupExport(host.UserData, "test.alpha", "alpha.ping", 1);
		void* tooNew = host.LookupExport(host.UserData, "test.alpha", "alpha.ping", 2);
		void* missingName = host.LookupExport(host.UserData, "test.alpha", "alpha.absent", 1);
		void* missingPlugin = host.LookupExport(host.UserData, "test.absent", "alpha.ping", 1);

		WeTestPlugin::Log(hit ? "lookup alpha.ping v1 hit" : "lookup alpha.ping v1 miss",
			hit ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log(tooNew ? "lookup alpha.ping v2 unexpectedly hit"
			: "lookup alpha.ping v2 miss (min version)", tooNew ? WePluginLogError : WePluginLogInfo);
		WeTestPlugin::Log(missingName ? "lookup alpha.absent unexpectedly hit"
			: "lookup alpha.absent miss", missingName ? WePluginLogError : WePluginLogInfo);
		WeTestPlugin::Log(missingPlugin ? "lookup test.absent unexpectedly hit"
			: "lookup test.absent miss", missingPlugin ? WePluginLogError : WePluginLogInfo);

		const bool ok = hit && !tooNew && !missingName && !missingPlugin;
		if (!ok)
			return false;
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
		plugin.Id = "test.lookup";
		plugin.Name = "Test Export Lookup";
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
