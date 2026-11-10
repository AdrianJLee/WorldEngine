// PLUG-T2a 正例:宿主注册面 —— 资产类型(含"新建"回调 + userData 回传)、
// 导入器(Matches / SettingsFingerprint / Import 走 C ABI + sink)、重复注册与幂等注销。
//
// 证据口径:插件侧只经 host.Log 回传结果(单测用 World::Log::RecentLines 取回),
// 宿主注册表/导入面由单测直接读。
#include "PluginTestSupport.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>

using namespace World::Plugins;

namespace
{
	char kMarker[] = "created-by-test.hostapi";   // Create 的 userData:内容写进产物 → 证明原样回传

	constexpr const char* kAssetTypeId = "test.hostapi.type";
	constexpr const char* kImporterId = "test.hostapi.importer";
	constexpr uint64_t kSettingsFingerprint = 7;

	void SetError(char* buffer, uint32_t capacity, const std::string& text)
	{
		if (!buffer || capacity == 0)
			return;
		const size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
		std::memcpy(buffer, text.data(), count);
		buffer[count] = '\0';
	}

	bool CreateAsset(void* userData, const char* directoryUtf8, char* errorBuffer,
		uint32_t errorCapacity)
	{
		const auto* marker = static_cast<const char*>(userData);
		if (!directoryUtf8 || !directoryUtf8[0] || !marker)
		{
			SetError(errorBuffer, errorCapacity, "missing directory or userData");
			return false;
		}
		std::ofstream out(std::string(directoryUtf8) + "/hostapi.created",
			std::ios::binary | std::ios::trunc);
		if (!out)
		{
			SetError(errorBuffer, errorCapacity, "cannot create hostapi.created");
			return false;
		}
		out << marker;
		return out.good();
	}

	bool MatchesSource(void* userData, const char* sourceUtf8)
	{
		(void)userData;
		if (!sourceUtf8)
			return false;
		const std::string_view path(sourceUtf8);
		constexpr std::string_view suffix = ".whostapi";
		return path.size() > suffix.size()
			&& path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	uint64_t SettingsFingerprint(void* userData, const char* sourceUtf8)
	{
		(void)userData;
		(void)sourceUtf8;
		return kSettingsFingerprint;
	}

	bool ImportSource(void* userData, const char* logicalPathUtf8, const char* sourceUtf8,
		const WeImportSink* sink, char* errorBuffer, uint32_t errorCapacity)
	{
		(void)userData;
		(void)logicalPathUtf8;
		if (!sink || !sink->Write || !sink->UserData || !sourceUtf8)
		{
			SetError(errorBuffer, errorCapacity, "missing sink or source");
			return false;
		}
		std::ifstream in(sourceUtf8, std::ios::binary);
		if (!in)
		{
			SetError(errorBuffer, errorCapacity, "cannot open source");
			return false;
		}
		std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const std::string product = "IMPORTED:" + bytes;
		if (!sink->Write(sink->UserData, nullptr, product.data(),
			static_cast<uint64_t>(product.size())))
		{
			SetError(errorBuffer, errorCapacity, "host rejected the product");
			return false;
		}
		return true;
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
		type.Label = "Host API Test Type";
		type.Extension = ".whostapi";
		type.SortOrder = 77;
		type.Create = &CreateAsset;
		type.UserData = kMarker;
		const bool typeOk = host.RegisterAssetType(host.UserData, &type);
		const bool typeDuplicate = host.RegisterAssetType(host.UserData, &type);
		WeTestPlugin::Log(typeOk ? "asset-type-registered" : "asset-type-register-failed",
			typeOk ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log(typeDuplicate ? "asset-type-duplicate-accepted"
			: "asset-type-duplicate-rejected", WePluginLogInfo);

		WeAssetImporterDesc importer;
		importer.Id = kImporterId;
		importer.DisplayName = "Host API Test Importer";
		importer.Version = 3;
		importer.Matches = &MatchesSource;
		importer.Import = &ImportSource;
		importer.SettingsFingerprint = &SettingsFingerprint;
		const bool importerOk = host.RegisterAssetImporter(host.UserData, &importer);
		const bool importerDuplicate = host.RegisterAssetImporter(host.UserData, &importer);
		WeTestPlugin::Log(importerOk ? "importer-registered" : "importer-register-failed",
			importerOk ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log(importerDuplicate ? "importer-duplicate-accepted"
			: "importer-duplicate-rejected", WePluginLogInfo);

		if (!typeOk || !importerOk)
		{
			if (importerOk)
				host.UnregisterAssetImporter(host.UserData, kImporterId);
			if (typeOk)
				host.UnregisterAssetType(host.UserData, kAssetTypeId);
			return false;
		}
		WeTestPlugin::Log("registered");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		const WeHostApi& host = WeTestPlugin::Host();
		const bool importerFirst = host.UnregisterAssetImporter(host.UserData, kImporterId);
		const bool importerSecond = host.UnregisterAssetImporter(host.UserData, kImporterId);
		const bool typeFirst = host.UnregisterAssetType(host.UserData, kAssetTypeId);
		const bool typeSecond = host.UnregisterAssetType(host.UserData, kAssetTypeId);
		WeTestPlugin::Log((importerFirst && importerSecond) ? "importer-unregistered idempotent"
			: "importer-unregister-failed", (importerFirst && importerSecond) ? WePluginLogInfo
				: WePluginLogError);
		WeTestPlugin::Log((typeFirst && typeSecond) ? "asset-type-unregistered idempotent"
			: "asset-type-unregister-failed", (typeFirst && typeSecond) ? WePluginLogInfo
				: WePluginLogError);
		WeTestPlugin::Log("unregistered");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.hostapi";
		plugin.Name = "Test Host API";
		plugin.Version = "1.0.0";
		plugin.Provides = nullptr;
		plugin.ProvidesCount = 0;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
