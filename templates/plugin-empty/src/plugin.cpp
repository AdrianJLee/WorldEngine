// {{PluginName}} — 空插件骨架(Empty Plugin 模板,{{PluginScope}} 形态)。
//
// 这是最小可加载插件:导出 WePluginQuery + Register/Unregister,不做任何注册。
// 要加功能:在 Register 里注册、在 Unregister 里对称地注销。
// 只依赖公共 ABI 头(World/Plugins/WePluginApi.h),不链接 World。
#include "World/Plugins/WePluginApi.h"

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

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		// 空模板:还没有任何要注册的东西 —— 从这里开始加。
		if (host.Log && host.UserData)
			host.Log(host.UserData, WePluginLogInfo, "loaded (empty template)");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		// Register 里注册了什么,就在这里对称地注销什么。
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = kPluginId;
		plugin.Name = kPluginName;
		plugin.Version = "1.0.0";
		plugin.MinEngineVersion = ">=2.0";
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
