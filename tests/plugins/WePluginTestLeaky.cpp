// PLUG-T2a 负例:插件注册了资产类型 + 导入器,**故意不在 Unregister 里注销** ——
// 宿主必须在卸载时兜底回收(记警告)且不留下指向已释放 DLL 的悬空回调。
#include "PluginTestSupport.h"

#include <cstdint>
#include <string_view>

using namespace World::Plugins;

namespace
{
	constexpr const char* kAssetTypeId = "test.leaky.type";
	constexpr const char* kImporterId = "test.leaky.importer";

	bool MatchesSource(void* userData, const char* sourceUtf8)
	{
		(void)userData;
		if (!sourceUtf8)
			return false;
		const std::string_view path(sourceUtf8);
		constexpr std::string_view suffix = ".wleaky";
		return path.size() > suffix.size()
			&& path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	bool ImportSource(void* userData, const char* logicalPathUtf8, const char* sourceUtf8,
		const WeImportSink* sink, char* errorBuffer, uint32_t errorCapacity)
	{
		(void)userData;
		(void)logicalPathUtf8;
		(void)sourceUtf8;
		(void)errorBuffer;
		(void)errorCapacity;
		if (!sink || !sink->Write || !sink->UserData)
			return false;
		const char product[] = "LEAKY";
		return sink->Write(sink->UserData, nullptr, product, sizeof(product) - 1);
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host) || !WeTestPlugin::HostApiHasRegistrationSurface(host))
		{
			WeTestPlugin::Log("host registration surface missing; rejected", WePluginLogError);
			return false;
		}
		WeTestPlugin::Host() = host;   // WeTestPlugin::Log 走这张表回传插件侧证据

		WeAssetTypeDesc type;
		type.Id = kAssetTypeId;
		type.Label = "Leaky Type";
		type.Extension = ".wleaky";
		const bool typeOk = host.RegisterAssetType(host.UserData, &type);

		WeAssetImporterDesc importer;
		importer.Id = kImporterId;
		importer.Version = 1;
		importer.Matches = &MatchesSource;
		importer.Import = &ImportSource;
		const bool importerOk = host.RegisterAssetImporter(host.UserData, &importer);

		if (!typeOk || !importerOk)
		{
			WeTestPlugin::Log("leaky plugin registration failed", WePluginLogError);
			return false;
		}
		WeTestPlugin::Log("registered (will leak registrations)");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		// 故意不注销:验证宿主卸载兜底(警告 + 强制移除)。
		WeTestPlugin::Log("unregistered (registrations intentionally left behind)");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.leaky";
		plugin.Name = "Test Leaky Registrations";
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
