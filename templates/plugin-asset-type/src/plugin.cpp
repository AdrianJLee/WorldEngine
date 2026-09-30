// {{PluginName}} — 资产类型插件骨架(Asset Type Plugin 模板,{{PluginScope}} 形态)。
//
// 做什么(以 plugins/hello-import 为蓝本):
//   * 注册一个新资产类型({{PluginDir}} / .{{PluginDir}},含"新建"回调);
//   * 注册一个极简导入器({{PluginDir}}.asset),把源文件加一行头作为产物交给宿主。
// 只依赖公共 ABI 头(World/Plugins/WePluginApi.h),不链接 World。
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

	constexpr const char* kPluginId = "{{PluginId}}";
	constexpr const char* kPluginName = "{{PluginName}}";
	constexpr const char* kAssetTypeId = "{{PluginDir}}";
	constexpr const char* kImporterId = "{{PluginDir}}.asset";
	constexpr const char* kExtension = ".{{PluginDir}}";

	// 回调的 userData = 插件自己的句柄(这里就是插件身份串)。
	char kPluginTag[] = "{{PluginId}}";

	// Unregister 无参数:按 ABI 契约由插件自己记住宿主表。
	WeHostApi g_Host;

	// 插件侧的"双向校验"一半:**新插件对旧宿主**必须自检宿主表覆盖到 T2 最后一个字段,
	// 不覆盖 = 干净拒绝(不按旧布局解释)。
	bool HostHasRegistrationSurface(const WeHostApi& host)
	{
		return host.StructSize >= offsetof(WeHostApi, LookupExport) + sizeof(host.LookupExport)
			&& host.RegisterAssetType && host.UnregisterAssetType
			&& host.RegisterAssetImporter && host.UnregisterAssetImporter;
	}

	void SetError(char* buffer, uint32_t capacity, const std::string& text)
	{
		if (!buffer || capacity == 0)
			return;
		const size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
		std::memcpy(buffer, text.data(), count);
		buffer[count] = '\0';
	}

	// 资产"新建":在目标目录写一份模板(重名则 new-1 / new-2 …,不覆盖已有资产)。
	bool CreateAsset(void* userData, const char* directoryUtf8, char* errorBuffer,
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
				? std::string("new") + kExtension
				: ("new-" + std::to_string(index) + kExtension);
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
			out << "# created by " << (pluginTag ? pluginTag : "plugin") << "\n";
			if (!out.good())
			{
				SetError(errorBuffer, errorCapacity, "failed to write " + target.string());
				return false;
			}
			return true;
		}
		SetError(errorBuffer, errorCapacity, "too many existing assets in the target directory");
		return false;
	}

	// 导入器:按扩展名接手(Extensions 只是元数据,"谁接手"以本回调为准)。
	bool MatchesSource(void* userData, const char* sourceUtf8)
	{
		(void)userData;
		if (!sourceUtf8)
			return false;
		const std::string_view path(sourceUtf8);
		const std::string_view suffix(kExtension);
		return path.size() > suffix.size()
			&& path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	// 本导入器没有逐源设置 ⇒ 0(与 IAssetImporter::SettingsFingerprint 默认同语义)。
	uint64_t SettingsFingerprint(void* userData, const char* sourceUtf8)
	{
		(void)userData;
		(void)sourceUtf8;
		return 0;
	}

	// 导入:源字节加一行 "WEASSET1" 头,作为**单产物**写进宿主 sink。
	bool ImportSource(void* userData, const char* logicalPathUtf8, const char* sourceUtf8,
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
		std::string product = "WEASSET1\n";
		product += bytes;
		if (!sink->Write(sink->UserData, nullptr, product.data(),
			static_cast<uint64_t>(product.size())))
		{
			SetError(errorBuffer, errorCapacity, "host rejected the imported product");
			return false;
		}
		return true;
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!HostHasRegistrationSurface(host))
		{
			if (host.Log && host.UserData)
				host.Log(host.UserData, WePluginLogError,
					"host does not expose the plugin registration surface; rejected");
			return false;
		}
		g_Host = host;

		WeAssetTypeDesc type;
		type.Id = kAssetTypeId;
		type.Label = "{{PluginName}} Asset";
		type.Extension = kExtension;
		type.SortOrder = 120;
		type.Create = &CreateAsset;
		type.UserData = kPluginTag;
		const bool typeOk = host.RegisterAssetType(host.UserData, &type);

		static const char* const kExtensions[] = { kExtension };
		WeAssetImporterDesc importer;
		importer.Id = kImporterId;
		importer.DisplayName = "{{PluginName}} Source";
		importer.Extensions = kExtensions;
		importer.ExtensionCount = 1;
		importer.Version = 1;
		importer.Matches = &MatchesSource;
		importer.Import = &ImportSource;
		importer.SettingsFingerprint = &SettingsFingerprint;
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
			host.Log(host.UserData, WePluginLogInfo, "registered asset type + importer");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		if (g_Host.UnregisterAssetImporter && g_Host.UserData)
			g_Host.UnregisterAssetImporter(g_Host.UserData, kImporterId);
		if (g_Host.UnregisterAssetType && g_Host.UserData)
			g_Host.UnregisterAssetType(g_Host.UserData, kAssetTypeId);
	}

	const char* const kProvides[] = { "asset.type", "asset.importer" };

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = kPluginId;
		plugin.Name = kPluginName;
		plugin.Version = "1.0.0";
		plugin.MinEngineVersion = ">=2.0";
		plugin.Provides = kProvides;
		plugin.ProvidesCount = 2;
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
