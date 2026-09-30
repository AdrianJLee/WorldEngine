// PLUG-T2b 负例:注册了组件类型,**故意不在 Unregister 里注销** ——
// 宿主必须在卸载/管理器析构的兜底回收里注销整个 schema 模块(记警告),
// 不留"指向已卸载插件的类型"给编辑器/序列化看。
#include "PluginComponentFixture.h"
#include "PluginTestSupport.h"

#include <cstddef>
#include <cstdint>

using namespace World::Plugins;
using WePluginComponentFixture::ShieldFixture;

namespace
{
	constexpr const char* kLeakyId = "test.leakycomponent.Shield";

	const WeComponentFieldDesc* LeakyFields()
	{
		static const WeComponentFieldDesc fields[] =
		{
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Shield", WeComponentKindFloat,
				static_cast<uint32_t>(offsetof(ShieldFixture, Shield)), sizeof(float),
				WeComponentFieldFlagNone, nullptr, 0, nullptr },
		};
		return fields;
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host)
			|| !WeTestPlugin::HostApiHasComponentSurface(host))
		{
			WeTestPlugin::Log("host component surface missing; rejected", WePluginLogError);
			return false;
		}
		WeTestPlugin::Host() = host;

		WeComponentDesc desc;
		desc.Id = kLeakyId;
		desc.DisplayName = "Leaky Component";
		desc.Fields = LeakyFields();
		desc.FieldCount = 1;
		if (!host.RegisterComponent(host.UserData, &desc))
		{
			WeTestPlugin::Log("leaky component registration failed", WePluginLogError);
			return false;
		}
		WeTestPlugin::Log("registered (component will leak)");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		// 故意不注销:验证宿主卸载兜底(警告 + 整模块移除 + 槽位释放)。
		WeTestPlugin::Log("unregistered (component registration intentionally left behind)");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.leakycomponent";
		plugin.Name = "Test Leaky Component Schema";
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
