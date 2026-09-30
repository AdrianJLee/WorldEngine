// PLUG-T2c 正例:带 entt 存储桥的组件注册(WeComponentDesc::Size / Alignment)。
//
// 覆盖(插件侧证据全部经 host.Log 回传,由 World.Plugins 单测取回):
//   * 带 Size/Alignment 的两个组件注册成功(宿主合成 blob 存储,能挂到场景实体上);
//   * 坏声明被干净拒绝:Size 超宿主最大档 / Alignment 不是 2 的幂 / Alignment 超宿主上限 /
//     Size 不覆盖字段 / Size=0 但 Alignment!=0(不半注册);
//   * 导出 `storage.unregister`:单测按需注销**单个**类型,观察"注销一个类型后同模块其余
//     类型仍在";
//   * Unregister 里按契约再注销一次 = 幂等。
#include "PluginComponentFixture.h"
#include "PluginTestSupport.h"

#include <cstddef>
#include <cstdint>

using namespace World::Plugins;
using WePluginComponentFixture::HealthFixture;
using WePluginComponentFixture::ShieldFixture;

namespace
{
	constexpr const char* kHealthId = "test.storage.Health";
	constexpr const char* kShieldId = "test.storage.Shield";

	const char* const kTierChoices[] = { "light", "heavy" };
	constexpr const char* kLegacyId = "test.storage.Legacy";

	// 字段布局:Offset/Size 由 offsetof/sizeof 给出(fixture 头与单测共用同一份布局);
	// 结构总大小/对齐由 WeComponentDesc::Size / Alignment 声明(T2c)。
	const WeComponentFieldDesc* HealthFields()
	{
		static const WeComponentFieldDesc fields[] =
		{
			{ sizeof(WeComponentFieldDesc), WE_PLUGIN_ABI_VERSION, "Enabled", WeComponentKindBool,
				static_cast<uint32_t>(offsetof(HealthFixture, Enabled)), sizeof(bool),
				WeComponentFieldFlagNone, nullptr, 0, "Storage-backed switch." },
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

	// 坏声明必须被宿主干净拒绝(单测按宿主诊断断言;插件侧只看返回值,避免日志刷屏 ——
	// World.Plugins 的"最近日志"缓冲只有 400 行,断言依赖它)。
	bool RegisterBadDescriptors(const WeHostApi& host)
	{
		// ① 声明大小超过宿主最大档(2048)。
		WeComponentDesc tooBig;
		tooBig.Id = "test.storage.TooBig";
		tooBig.Size = 4096;
		tooBig.Alignment = 16;
		tooBig.Fields = ShieldFields();
		tooBig.FieldCount = 1;
		const bool tooBigOk = host.RegisterComponent(host.UserData, &tooBig);

		// ② 对齐不是 2 的幂(24)。
		WeComponentDesc badAlignment;
		badAlignment.Id = "test.storage.BadAlignment";
		badAlignment.Size = 48;
		badAlignment.Alignment = 24;
		badAlignment.Fields = ShieldFields();
		badAlignment.FieldCount = 1;
		const bool badAlignmentOk = host.RegisterComponent(host.UserData, &badAlignment);

		// ③ 对齐超过宿主 blob 上限(16)。
		WeComponentDesc hugeAlignment;
		hugeAlignment.Id = "test.storage.HugeAlignment";
		hugeAlignment.Size = 64;
		hugeAlignment.Alignment = 32;
		hugeAlignment.Fields = ShieldFields();
		hugeAlignment.FieldCount = 1;
		const bool hugeAlignmentOk = host.RegisterComponent(host.UserData, &hugeAlignment);

		// ④ 声明大小不覆盖字段(HealthFixture 的 Offset/Tier 都超出 8 字节)。
		WeComponentDesc shortSize;
		shortSize.Id = "test.storage.ShortSize";
		shortSize.Size = 8;
		shortSize.Alignment = 4;
		shortSize.Fields = HealthFields();
		shortSize.FieldCount = 5;
		const bool shortSizeOk = host.RegisterComponent(host.UserData, &shortSize);

		// ⑤ Size=0 但声明了对齐 = 语义矛盾(0 表示 schema-only,不该有存储语义)。
		WeComponentDesc alignmentWithoutSize;
		alignmentWithoutSize.Id = "test.storage.AlignmentWithoutSize";
		alignmentWithoutSize.Alignment = 4;
		alignmentWithoutSize.Fields = ShieldFields();
		alignmentWithoutSize.FieldCount = 1;
		const bool alignmentWithoutSizeOk = host.RegisterComponent(host.UserData, &alignmentWithoutSize);

		return !tooBigOk && !badAlignmentOk && !hugeAlignmentOk && !shortSizeOk
			&& !alignmentWithoutSizeOk;
	}

	// 导出给单测:按 id 注销单个组件类型(宿主表在加载期间一直有效 —— 单测在 Unload 前调用)。
	bool UnregisterComponentById(const char* id)
	{
		const WeHostApi& host = WeTestPlugin::Host();
		if (!id || !host.UserData || !host.UnregisterComponent)
			return false;
		const bool ok = host.UnregisterComponent(host.UserData, id);
		WeTestPlugin::Log(ok ? "storage-export-unregister-ok" : "storage-export-unregister-failed",
			ok ? WePluginLogInfo : WePluginLogError);
		return ok;
	}

	// 正例组件注册(Register 与导出回调共用同一份声明)。
	bool RegisterHealth(const WeHostApi& host)
	{
		WeComponentDesc health;
		health.Id = kHealthId;
		health.DisplayName = "Plugin Storage Health";
		health.Fields = HealthFields();
		health.FieldCount = 5;
		health.Size = static_cast<uint32_t>(sizeof(HealthFixture));
		health.Alignment = static_cast<uint32_t>(alignof(HealthFixture));
		const bool ok = host.RegisterComponent(host.UserData, &health);
		WeTestPlugin::Log(ok ? "storage-health-registered" : "storage-health-register-failed",
			ok ? WePluginLogInfo : WePluginLogError);
		return ok;
	}

	// 导出给单测:重新注册 Health(观察槽位/存储 id 复用;宿主表在加载期间一直有效)。
	bool RegisterHealthAgain()
	{
		const WeHostApi& host = WeTestPlugin::Host();
		if (!host.UserData || !host.RegisterComponent)
			return false;
		const bool ok = RegisterHealth(host);
		WeTestPlugin::Log(ok ? "storage-export-register-ok" : "storage-export-register-failed",
			ok ? WePluginLogInfo : WePluginLogError);
		return ok;
	}

	const WePluginExport kExports[] =
	{
		{ "storage.unregister", 1, reinterpret_cast<void*>(&UnregisterComponentById) },
		{ "storage.register-health", 1, reinterpret_cast<void*>(&RegisterHealthAgain) },
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
			WeTestPlugin::Log("bad storage descriptors were not rejected", WePluginLogError);
			return false;
		}

		const bool healthOk = RegisterHealth(host);

		WeComponentDesc shield;
		shield.Id = kShieldId;
		shield.DisplayName = "Plugin Storage Shield";
		shield.Fields = ShieldFields();
		shield.FieldCount = 1;
		shield.Size = static_cast<uint32_t>(sizeof(ShieldFixture));
		shield.Alignment = static_cast<uint32_t>(alignof(ShieldFixture));
		const bool shieldOk = host.RegisterComponent(host.UserData, &shield);
		WeTestPlugin::Log(shieldOk ? "storage-shield-registered" : "storage-shield-register-failed",
			shieldOk ? WePluginLogInfo : WePluginLogError);

		// 旧式 schema-only 组件(Size/Alignment 都是 0)= T2b 行为,必须与存储桥共存且不退化。
		WeComponentDesc legacy;
		legacy.Id = kLegacyId;
		legacy.DisplayName = "Plugin Legacy Schema";
		legacy.Fields = ShieldFields();
		legacy.FieldCount = 1;
		legacy.Size = 0;
		legacy.Alignment = 0;
		const bool legacyOk = host.RegisterComponent(host.UserData, &legacy);
		WeTestPlugin::Log(legacyOk ? "storage-legacy-registered" : "storage-legacy-register-failed",
			legacyOk ? WePluginLogInfo : WePluginLogError);

		if (!healthOk || !shieldOk || !legacyOk)
			return false;
		WeTestPlugin::Log("registered");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		const WeHostApi& host = WeTestPlugin::Host();
		if (!host.UserData || !host.UnregisterComponent)
		{
			WeTestPlugin::Log("storage unregister surface missing", WePluginLogError);
			return;
		}
		// 可能已被导出回调注销(单测的"只注销一个"路径)⇒ 幂等,仍然返回 true。
		const bool healthFirst = host.UnregisterComponent(host.UserData, kHealthId);
		const bool healthSecond = host.UnregisterComponent(host.UserData, kHealthId);
		const bool shieldFirst = host.UnregisterComponent(host.UserData, kShieldId);
		const bool shieldSecond = host.UnregisterComponent(host.UserData, kShieldId);
		const bool legacyFirst = host.UnregisterComponent(host.UserData, kLegacyId);
		const bool legacySecond = host.UnregisterComponent(host.UserData, kLegacyId);
		const bool ok = healthFirst && healthSecond && shieldFirst && shieldSecond
			&& legacyFirst && legacySecond;
		WeTestPlugin::Log(ok ? "storage-unregistered idempotent" : "storage-unregister-failed",
			ok ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log("unregistered");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.storage";
		plugin.Name = "Test Component Storage Bridge";
		plugin.Version = "1.0.0";
		plugin.Exports = kExports;
		plugin.ExportCount = 2;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
