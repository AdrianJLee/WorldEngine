// PLUG-T2b 正例:组件 schema 注册(WeHostApi::RegisterComponent / UnregisterComponent)。
//
// 覆盖(插件侧证据全部经 host.Log 回传,由 World.Plugins 单测取回):
//   * 两个组件类型注册成功(多字段布局 + 单字段),同类型重复注册被拒(不覆盖);
//   * 坏描述被干净拒绝:不支持的 Kind / Size 与 Kind 不符 / ComponentId != 0(无存储桥);
//   * 导出 `component.unregister`(单测经 LookupExport 调用)—— 按需注销**单个**类型,
//     让单测观察到"注销一个类型后同模块其余类型仍在"这条路径;
//   * Unregister 里按契约再注销一次 = 幂等(与导出路径不冲突)。
#include "PluginComponentFixture.h"
#include "PluginTestSupport.h"

#include <cstddef>
#include <cstdint>

using namespace World::Plugins;
using WePluginComponentFixture::HealthFixture;
using WePluginComponentFixture::ShieldFixture;

namespace
{
	constexpr const char* kHealthId = "test.component.Health";
	constexpr const char* kShieldId = "test.component.Shield";

	const char* const kTierChoices[] = { "light", "heavy" };

	// 字段布局:Offset/Size 由 offsetof/sizeof 直接给出(fixture 头与单测共用同一份布局)。
	const WeComponentFieldDesc* HealthFields()
	{
		static const WeComponentFieldDesc fields[] =
		{
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Enabled", WeComponentKindBool,
				static_cast<uint32_t>(offsetof(HealthFixture, Enabled)), sizeof(bool),
				WeComponentFieldFlagReadOnly, nullptr, 0, "Read-only switch for the test fixture." },
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Charges", WeComponentKindInt32,
				static_cast<uint32_t>(offsetof(HealthFixture, Charges)), sizeof(int32_t),
				WeComponentFieldFlagNone, nullptr, 0, nullptr },
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Health", WeComponentKindFloat,
				static_cast<uint32_t>(offsetof(HealthFixture, Health)), sizeof(float),
				WeComponentFieldFlagNone, nullptr, 0, "Hit points." },
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Offset", WeComponentKindVec3,
				static_cast<uint32_t>(offsetof(HealthFixture, Offset)), sizeof(float) * 3,
				WeComponentFieldFlagColor, nullptr, 0, nullptr },
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Tier", WeComponentKindUInt8,
				static_cast<uint32_t>(offsetof(HealthFixture, Tier)), sizeof(uint8_t),
				WeComponentFieldFlagNone, kTierChoices, 2, nullptr },
		};
		return fields;
	}

	bool RegisterBadDescriptors(const WeHostApi& host)
	{
		// ① 不支持的 Kind(String)—— 干净拒绝(不半注册)。
		const WeComponentFieldDesc unsupported[] =
		{
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Label", WeComponentKindString,
				0, static_cast<uint32_t>(sizeof(const char*)), WeComponentFieldFlagNone, nullptr, 0, nullptr },
		};
		WeComponentDesc unsupportedDesc;
		unsupportedDesc.Id = "test.component.Unsupported";
		unsupportedDesc.Fields = unsupported;
		unsupportedDesc.FieldCount = 1;
		const bool unsupportedOk = host.RegisterComponent(host.UserData, &unsupportedDesc);
		WeTestPlugin::Log(unsupportedOk ? "unsupported-kind-accepted" : "unsupported-kind-rejected",
			unsupportedOk ? WePluginLogError : WePluginLogInfo);

		// ② Size 与 Kind 不符(Float 声明 8 字节)—— 干净拒绝。
		const WeComponentFieldDesc sizeMismatch[] =
		{
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Health", WeComponentKindFloat,
				0, static_cast<uint32_t>(sizeof(double)), WeComponentFieldFlagNone, nullptr, 0, nullptr },
		};
		WeComponentDesc sizeDesc;
		sizeDesc.Id = "test.component.SizeMismatch";
		sizeDesc.Fields = sizeMismatch;
		sizeDesc.FieldCount = 1;
		const bool sizeOk = host.RegisterComponent(host.UserData, &sizeDesc);
		WeTestPlugin::Log(sizeOk ? "size-mismatch-accepted" : "size-mismatch-rejected",
			sizeOk ? WePluginLogError : WePluginLogInfo);

		// ③ ComponentId != 0 = 要求存储桥(T2b 没有)—— 干净拒绝。
		WeComponentDesc storageDesc;
		storageDesc.Id = "test.component.Storage";
		storageDesc.ComponentId = 0x1234;
		storageDesc.Fields = HealthFields();
		storageDesc.FieldCount = 5;
		const bool storageOk = host.RegisterComponent(host.UserData, &storageDesc);
		WeTestPlugin::Log(storageOk ? "component-id-accepted" : "component-id-rejected",
			storageOk ? WePluginLogError : WePluginLogInfo);

		return !unsupportedOk && !sizeOk && !storageOk;
	}

	const WeComponentFieldDesc* ShieldFields()
	{
		static const WeComponentFieldDesc fields[] =
		{
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Shield", WeComponentKindFloat,
				static_cast<uint32_t>(offsetof(ShieldFixture, Shield)), sizeof(float),
				WeComponentFieldFlagNone, nullptr, 0, nullptr },
		};
		return fields;
	}

	// 导出给单测:按 id 注销单个组件类型(宿主表在加载期间一直有效 —— 单测在 Unload 前调用)。
	bool UnregisterComponentById(const char* id)
	{
		const WeHostApi& host = WeTestPlugin::Host();
		if (!id || !host.UserData || !host.UnregisterComponent)
			return false;
		const bool ok = host.UnregisterComponent(host.UserData, id);
		WeTestPlugin::Log(ok ? "component-export-unregister-ok" : "component-export-unregister-failed",
			ok ? WePluginLogInfo : WePluginLogError);
		return ok;
	}

	const WePluginExport kExports[] =
	{
		{ "component.unregister", 1, reinterpret_cast<void*>(&UnregisterComponentById) },
	};

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host)
			|| !WeTestPlugin::HostApiHasComponentSurface(host))
		{
			WeTestPlugin::Log("host component surface missing; rejected", WePluginLogError);
			return false;
		}
		WeTestPlugin::Host() = host;   // WeTestPlugin::Log 与导出回调都走这张表

		if (!RegisterBadDescriptors(host))
		{
			WeTestPlugin::Log("bad component descriptors were not rejected", WePluginLogError);
			return false;
		}

		WeComponentDesc health;
		health.Id = kHealthId;
		health.DisplayName = "Plugin Health";
		health.Fields = HealthFields();
		health.FieldCount = 5;
		const bool healthOk = host.RegisterComponent(host.UserData, &health);
		const bool healthDuplicate = host.RegisterComponent(host.UserData, &health);
		WeTestPlugin::Log(healthOk ? "component-registered" : "component-register-failed",
			healthOk ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log(healthDuplicate ? "component-duplicate-accepted"
			: "component-duplicate-rejected", WePluginLogInfo);

		WeComponentDesc shield;
		shield.Id = kShieldId;
		shield.DisplayName = "Plugin Shield";
		shield.Fields = ShieldFields();
		shield.FieldCount = 1;
		const bool shieldOk = host.RegisterComponent(host.UserData, &shield);
		WeTestPlugin::Log(shieldOk ? "shield-registered" : "shield-register-failed",
			shieldOk ? WePluginLogInfo : WePluginLogError);

		if (!healthOk || !shieldOk)
			return false;
		WeTestPlugin::Log("registered");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		const WeHostApi& host = WeTestPlugin::Host();
		if (!host.UserData || !host.UnregisterComponent)
		{
			WeTestPlugin::Log("component unregister surface missing", WePluginLogError);
			return;
		}
		// 可能已被导出回调注销(单测的"只注销一个"路径)⇒ 幂等,仍然返回 true。
		const bool healthFirst = host.UnregisterComponent(host.UserData, kHealthId);
		const bool healthSecond = host.UnregisterComponent(host.UserData, kHealthId);
		const bool shieldFirst = host.UnregisterComponent(host.UserData, kShieldId);
		const bool shieldSecond = host.UnregisterComponent(host.UserData, kShieldId);
		const bool ok = healthFirst && healthSecond && shieldFirst && shieldSecond;
		WeTestPlugin::Log(ok ? "component-unregistered idempotent" : "component-unregister-failed",
			ok ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log("unregistered");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.component";
		plugin.Name = "Test Component Schema";
		plugin.Version = "1.0.0";
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
