// PLUG-T1 正常插件 A:无依赖,声明 C++ 导出表;Register/Unregister 都经宿主表日志回传证据。
#include "PluginTestSupport.h"

using namespace World::Plugins;

namespace
{
	int AlphaPing()
	{
		return 42;
	}

	const WePluginExport kExports[] =
	{
		{ "alpha.ping", 1, reinterpret_cast<void*>(&AlphaPing) },
	};

	// 结构体声明的 provides 比清单多一项(test.extra)→ 宿主按契约给"缺声明"警告(不拒绝)。
	const char* const kProvides[] = { "cxx.exports", "test.extra" };

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
		plugin.Id = "test.alpha";
		plugin.Name = "Test Alpha";
		plugin.Version = "1.0.0";
		plugin.Publisher = "WorldEngine tests";
		plugin.MinEngineVersion = ">=2.0";
		plugin.Provides = kProvides;
		plugin.ProvidesCount = 2;
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
