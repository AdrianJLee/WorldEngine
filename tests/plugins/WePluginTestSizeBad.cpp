// PLUG-T1 负例:StructSize 与宿主不等值(ABI 版本正确)→ 必须被干净拒绝,且不读后面的字段。
#include "PluginTestSupport.h"

using namespace World::Plugins;

namespace
{
	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.size";
		plugin.Name = "Test Struct Size Mismatch";
		plugin.StructSize = static_cast<uint32_t>(sizeof(WePlugin) - 8);
		plugin.Register = nullptr;   // 到不了这一步:StructSize 在 Register 之前校验
		plugin.Unregister = nullptr;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
