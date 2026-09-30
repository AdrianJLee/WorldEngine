// {{PluginName}} — 场景组件插件骨架(Component Plugin 模板,{{PluginScope}} 形态)。
//
// 现状:**可编译骨架**。插件侧组件 schema 注册面属于 T2b,当前 ABI 还没有对应的
// 注册函数 —— 所以这里只声明能力 + 留 TODO,绝不引用尚不存在的 API。
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
		// TODO(T2b):插件侧组件 schema 注册面尚未就绪。落地后在这里注册组件,例如:
		//   WeComponentDesc component;
		//   component.Id = "{{PluginDir}}.Health";
		//   component.Fields = kFields;   // 字段描述表(名字/类型/默认值)
		//   host.RegisterComponentSchema(host.UserData, &component);
		// 并在 Unregister 里对称地注销(host.UnregisterComponentSchema)。
		if (host.Log && host.UserData)
			host.Log(host.UserData, WePluginLogInfo,
				"component skeleton loaded; schema registration waits for T2b");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		// TODO(T2b):与 Register 的注册成对。
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
