// PLUG-T1 正例:StructSize **大于**宿主已知前缀(模拟"插件用更新的头编译,尾部追加字段,
// abi 不变")⇒ 必须被接受并按已知前缀正常注册。这是 WePluginApi.h「字段只增不改号」的活体回归。
#include "PluginTestSupport.h"

using namespace World::Plugins;

namespace
{
	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host))
			return false;
		WeTestPlugin::Host() = host;
		WeTestPlugin::Log("registered (larger struct accepted)");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		WeTestPlugin::Log("unregistered");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.size.large";
		plugin.Name = "Test Larger Struct";
		plugin.Version = "1.0.0";
		// 模拟"更新的头":结构更大,但宿主只读 v1 前缀。
		plugin.StructSize = static_cast<uint32_t>(sizeof(WePlugin) + 32);
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
