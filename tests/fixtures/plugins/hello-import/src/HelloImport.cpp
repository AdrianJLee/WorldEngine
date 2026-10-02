// PLUG-T2 引擎插件示例:在 T1 的 ABI 之上真正"做点事"。
//   * 注册一个资产类型(hello / .whello,含"新建"回调);
//   * 注册一个极简导入器(给源加一行 WHELLO1 头,走既有 CookPipeline 导入面);
//   * 导出一个 C++ 函数库条目(hello.version)。
//
// 边界:本文件只依赖公共 ABI 头(不链接 World),与真实第三方插件一致;宿主侧由
// PluginManager 的 WeHostApi 注册面适配到 AssetTypeRegistry / 交给 CookPipeline 的导入器清单。
#include "World/Plugins/WePluginApi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#if defined(_WIN32)
#  define WE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define WE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
	using namespace World::Plugins;

	constexpr const char* kPluginId = "engine.hello-import";
	constexpr const char* kAssetTypeId = "hello";
	constexpr const char* kImporterId = "hello.whello";
	constexpr const char* kExportName = "hello.version";

	// 回调的 userData = 插件自己的句柄(这里就是插件身份串,新建回调用它写模板注释)。
	char kPluginTag[] = "engine.hello-import";

	// Unregister 无参数:按 ABI 契约由插件自己记住宿主表。
	WeHostApi g_Host;

	// 插件侧的"双向校验"一半:**新插件对旧宿主**必须自检宿主表覆盖到 T2 最后一个字段,
	// 不覆盖 = 干净拒绝(不按旧布局解释)。
	bool HostHasRegistrationSurface(const WeHostApi& host)
	{
		return host.StructSize >= offsetof(WeHostApi, LookupExport) + sizeof(host.LookupExport)
			&& host.RegisterAssetType && host.UnregisterAssetType
			&& host.RegisterAssetImporter && host.UnregisterAssetImporter
			&& host.LookupExport;
	}

	void SetError(char* buffer, uint32_t capacity, const std::string& text)
	{
		if (!buffer || capacity == 0)
			return;
		const size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
		std::memcpy(buffer, text.data(), count);
		buffer[count] = '\0';
	}

	// 资产"新建":在目标目录写一份 hello.whello 模板(重名则 hello-1.whello …,不覆盖已有资产)。
	bool CreateHelloAsset(void* userData, const char* directoryUtf8, char* errorBuffer,
		uint32_t errorCapacity)
	{
		const auto* pluginTag = static_cast<const char*>(userData);
		if (!directoryUtf8 || !directoryUtf8[0])
		{
			SetError(errorBuffer, errorCapacity, "missing target directory");
			return false;
		}

		const std::filesystem::path directory = std::filesystem::u8path(directoryUtf8);
		for (int index = 0; index < 1000; ++index)
		{
			const std::string name = index == 0
				? std::string("hello.whello")
				: ("hello-" + std::to_string(index) + ".whello");
			const std::filesystem::path target = directory / name;

			std::error_code existsError;
			if (std::filesystem::exists(target, existsError))
				continue;

			std::ofstream out(target, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				SetError(errorBuffer, errorCapacity, "cannot write " + target.string());
				return false;
			}
			out << "# hello asset created by " << (pluginTag ? pluginTag : "engine.hello-import") << "\n";
			if (!out.good())
			{
				SetError(errorBuffer, errorCapacity, "failed to write " + target.string());
				return false;
			}
			return true;
		}
		SetError(errorBuffer, errorCapacity, "too many existing hello assets in the target directory");
		return false;
	}

	// 导入器:按扩展名接手(.whello;扩展名列表只是元数据,"谁接手"以本回调为准)。
	bool MatchesHelloSource(void* userData, const char* sourceUtf8)
	{
		(void)userData;
		if (!sourceUtf8)
			return false;
		const std::string_view path(sourceUtf8);
		constexpr std::string_view suffix = ".whello";
		return path.size() > suffix.size()
			&& path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	// 本导入器没有逐源设置 ⇒ 0(与 IAssetImporter::SettingsFingerprint 默认同语义)。
	uint64_t HelloSettingsFingerprint(void* userData, const char* sourceUtf8)
	{
		(void)userData;
		(void)sourceUtf8;
		return 0;
	}

	// 导入:源字节加一行 "WHELLO1" 头,作为**单产物**写进宿主 sink(失败原因写 errorBuffer)。
	bool ImportHelloSource(void* userData, const char* logicalPathUtf8, const char* sourceUtf8,
		const WeImportSink* sink, char* errorBuffer, uint32_t errorCapacity)
	{
		(void)userData;
		(void)logicalPathUtf8;
		if (!sink || !sink->Write || !sink->UserData)
		{
			SetError(errorBuffer, errorCapacity, "host provided no import sink");
			return false;
		}
		if (!sourceUtf8 || !sourceUtf8[0])
		{
			SetError(errorBuffer, errorCapacity, "missing source path");
			return false;
		}

		std::ifstream in(std::filesystem::u8path(sourceUtf8), std::ios::binary);
		if (!in)
		{
			SetError(errorBuffer, errorCapacity, std::string("cannot open source: ") + sourceUtf8);
			return false;
		}
		const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::string product = "WHELLO1\n";
		product += bytes;
		if (!sink->Write(sink->UserData, nullptr, product.data(),
			static_cast<uint64_t>(product.size())))
		{
			SetError(errorBuffer, errorCapacity, "host rejected the imported product");
			return false;
		}
		return true;
	}

	// C++ 函数库导出:宿主/其它插件按 (id, name, minVersion) 经 WeHostApi::LookupExport 取得。
	uint32_t HelloVersion()
	{
		return 1;
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!HostHasRegistrationSurface(host))
		{
			if (host.Log && host.UserData)
				host.Log(host.UserData, WePluginLogError,
					"host does not expose the T2 registration surface; rejected");
			return false;
		}
		g_Host = host;

		WeAssetTypeDesc type;
		type.Id = kAssetTypeId;
		type.Label = "Hello File";
		type.Extension = ".whello";
		type.SortOrder = 120;
		type.Create = &CreateHelloAsset;
		type.UserData = kPluginTag;
		const bool typeOk = host.RegisterAssetType(host.UserData, &type);

		static const char* const kExtensions[] = { ".whello" };
		WeAssetImporterDesc importer;
		importer.Id = kImporterId;
		importer.DisplayName = "Hello (.whello)";
		importer.Extensions = kExtensions;
		importer.ExtensionCount = 1;
		importer.Version = 1;
		importer.Matches = &MatchesHelloSource;
		importer.Import = &ImportHelloSource;
		importer.SettingsFingerprint = &HelloSettingsFingerprint;
		importer.UserData = kPluginTag;
		const bool importerOk = host.RegisterAssetImporter(host.UserData, &importer);

		if (!typeOk || !importerOk)
		{
			// 契约:Register 返回 false = 插件必须已自行清理本次注册(不留半注册状态)。
			if (importerOk)
				host.UnregisterAssetImporter(host.UserData, kImporterId);
			if (typeOk)
				host.UnregisterAssetType(host.UserData, kAssetTypeId);
			if (host.Log && host.UserData)
				host.Log(host.UserData, WePluginLogError,
					"registration failed; partial registrations rolled back");
			return false;
		}

		if (host.Log && host.UserData)
			host.Log(host.UserData, WePluginLogInfo,
				"registered asset type 'hello' + importer 'hello.whello'");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		if (g_Host.UnregisterAssetImporter && g_Host.UserData)
			g_Host.UnregisterAssetImporter(g_Host.UserData, kImporterId);
		if (g_Host.UnregisterAssetType && g_Host.UserData)
			g_Host.UnregisterAssetType(g_Host.UserData, kAssetTypeId);
		if (g_Host.Log && g_Host.UserData)
			g_Host.Log(g_Host.UserData, WePluginLogInfo, "unregistered asset type + importer");
	}

	const char* const kProvides[] = { "asset.type", "asset.importer", "cxx.exports" };
	const WePluginExport kExports[] =
	{
		{ kExportName, 1, reinterpret_cast<void*>(&HelloVersion) },
	};

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = kPluginId;
		plugin.Name = "Hello Import";
		plugin.Version = "1.0.0";
		plugin.MinEngineVersion = ">=2.0";
		plugin.Provides = kProvides;
		plugin.ProvidesCount = 3;
		plugin.Exports = kExports;
		plugin.ExportCount = 1;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

// 入口:宿主 ABI 不等值 = 显式返回 nullptr(干净拒绝,不猜布局)。
WE_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	if (hostAbiVersion != WE_PLUGIN_ABI_VERSION)
		return nullptr;
	static const WePlugin plugin = MakePlugin();
	return &plugin;
}
