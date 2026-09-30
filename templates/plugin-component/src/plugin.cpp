// {{PluginName}} — 场景组件插件(Component Plugin 模板,{{PluginScope}} 形态)。
//
// T2b(2026-09-30)起是**真实注册**:Register 里把组件布局(每个字段的 Kind / Offset /
// Size,用 offsetof/sizeof 填写)经 WeHostApi::RegisterComponent 交给宿主,宿主据此在
// Schema::SchemaRegistry 里生成一个 TypeCategory::Component 类型;Unregister 成对注销。
//
// 语义边界:注册产物 = 组件 **schema**(注册表 Find/ListByModule 与序列化 API 可见)。
// 组件的 entt 存储桥不在本版 ABI 里 ⇒ `WeComponentDesc::ComponentId` 必须为 0,组件暂时
// 不能挂到场景实体上(将来在 ABI 尾部追加存储回调后才启用)。
// 只依赖公共 ABI 头(World/Plugins/WePluginApi.h),不链接 World。
#include "World/Plugins/WePluginApi.h"

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  define WE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define WE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
	using namespace World::Plugins;

	// 必须与 plugin.we.yaml 的 id 一致(宿主以清单为准;不一致 = 拒绝加载)。
	constexpr const char* kPluginId = "{{PluginId}}";
	constexpr const char* kPluginName = "{{PluginName}}";

	// 组件数据:布局由插件拥有;注册时用 offsetof/sizeof 告诉宿主每个字段在哪。
	struct HealthComponent
	{
		float Health = 100.0f;
		int32_t Charges = 3;
	};

	const char* const kChargeChoices[] = { "low", "normal", "high" };

	// 字段描述表(必须是静态存储:宿主注册期间只读,不在注册后保留指针,但保持简单可靠)。
	const WeComponentFieldDesc* ComponentFields()
	{
		static const WeComponentFieldDesc fields[] =
		{
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Health", WeComponentKindFloat,
				static_cast<uint32_t>(offsetof(HealthComponent, Health)), sizeof(float),
				WeComponentFieldFlagNone, nullptr, 0, "Hit points." },
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Charges", WeComponentKindInt32,
				static_cast<uint32_t>(offsetof(HealthComponent, Charges)), sizeof(int32_t),
				WeComponentFieldFlagNone, kChargeChoices, 3, nullptr },
		};
		return fields;
	}

	// Unregister 无宿主表参数(ABI 契约)⇒ 插件自己记住 Register 时拿到的那张表。
	WeHostApi& Host()
	{
		static WeHostApi host;
		return host;
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		// 双向校验:宿主表必须覆盖到组件注册面(尾部追加字段的 offsetof + sizeof 判据)。
		if (host.StructSize < offsetof(WeHostApi, UnregisterComponent) + sizeof(host.UnregisterComponent)
			|| host.AbiVersion != WE_PLUGIN_ABI_VERSION
			|| !host.Log || !host.RegisterComponent || !host.UnregisterComponent)
		{
			return false;   // 宿主太旧 / 缺能力 = 干净拒绝,不半注册
		}
		Host() = host;

		WeComponentDesc desc;
		desc.Id = "{{PluginId}}.Health";     // 类型全名(也是 .wd 里的类型键)
		desc.DisplayName = "{{PluginName}} Health";
		desc.ComponentId = 0;                // 没有存储桥 ⇒ 必须为 0(schema-only)
		desc.Fields = ComponentFields();
		desc.FieldCount = 2;
		if (!host.RegisterComponent(host.UserData, &desc))
		{
			host.Log(host.UserData, WePluginLogError, "component registration failed");
			return false;
		}
		host.Log(host.UserData, WePluginLogInfo, "registered {{PluginId}}.Health");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		const WeHostApi& host = Host();
		if (!host.UserData || !host.UnregisterComponent)
			return;
		host.UnregisterComponent(host.UserData, "{{PluginId}}.Health");   // 与 Register 成对
	}

	const char* const kProvides[] = { "scene.component" };

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = kPluginId;
		plugin.Name = kPluginName;
		plugin.Version = "1.0.0";
		plugin.MinEngineVersion = ">=2.0";
		plugin.Provides = kProvides;
		plugin.ProvidesCount = 1;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

// 入口:宿主 ABI 不等值 = 干净拒绝(不按旧布局解释)。
WE_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	if (hostAbiVersion != WE_PLUGIN_ABI_VERSION)
		return nullptr;
	static const WePlugin plugin = MakePlugin();
	return &plugin;
}
