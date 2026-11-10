// {{PluginName}} — C++ 函数库插件骨架(C++ Library Plugin 模板,{{PluginScope}} 形态)。
//
// 做什么:导出一张 C++ 函数表(provides: cxx.exports),宿主/其它插件按
// (插件 id, 名字, 最低版本) 经 WeHostApi::LookupExport 取得函数指针。
// 只依赖公共 ABI 头(World/Plugins/WePluginApi.h),不链接 World。
#include "World/Plugins/WePluginApi.h"

#include <cstdint>
#include <cstring>

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
	// 导出名带插件短名,避免不同插件互相撞名。
	constexpr const char* kExportName = "{{PluginDir}}.ping";

	// 示例导出函数。真实插件在这里放自己的 API(只传 C 类型/函数指针)。
	uint32_t Ping()
	{
		return 1;
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (host.Log && host.UserData)
			host.Log(host.UserData, WePluginLogInfo, "registered cxx.exports");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		// 本模板没有登记任何需要回收的宿主侧状态(导出表随 DLL 生命周期)。
	}

	const char* const kProvides[] = { "cxx.exports" };
	const WePluginExport kExports[] =
	{
		{ kExportName, 1, reinterpret_cast<void*>(&Ping) },
	};

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = kPluginId;
		plugin.Name = kPluginName;
		plugin.Version = "1.0.0";
		plugin.MinEngineVersion = ">=2.0";
		plugin.Provides = kProvides;
		plugin.ProvidesCount = 1;
		plugin.Exports = kExports;
		plugin.ExportCount = 1;
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
