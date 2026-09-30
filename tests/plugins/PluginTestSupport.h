#pragma once

// PLUG-T1:测试插件 DLL 共用的最小骨架。
//
// 约束:测试插件**不链接 World**(只依赖公共 ABI 头 WePluginApi.h)—— 加载器通过
// DynamicLibrary 显式取符号,和真实第三方插件的边界一致;每个插件 .cpp 单独编成一个 DLL,
// 所以头文件里的 inline 静态状态天然是"每 DLL 一份"。

#include "World/Plugins/WePluginApi.h"

#include <cstdint>

#if defined(_WIN32)
#  define WE_TEST_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define WE_TEST_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace WeTestPlugin
{
	using World::Plugins::WeHostApi;
	using World::Plugins::WePlugin;
	using World::Plugins::WePluginLogInfo;

	// 宿主在 Register 里给的 WeHostApi(Unregister 无参数,按契约由插件自己记住)。
	inline WeHostApi& Host()
	{
		static WeHostApi host;
		return host;
	}

	// 插件侧的"双向校验"一半:宿主表小了 / ABI 不等值 / 没有日志 = 拒绝注册
	// (不按旧布局解释宿主表)。
	inline bool HostApiIsCompatible(const WeHostApi& host)
	{
		return host.StructSize >= sizeof(WeHostApi)
			&& host.AbiVersion == World::Plugins::WE_PLUGIN_ABI_VERSION
			&& host.Log != nullptr;
	}

	// 通过宿主表日志回传插件侧证据(单测用 World::Log::RecentLines 取回)。
	inline void Log(const char* message, int level = WePluginLogInfo)
	{
		const WeHostApi& host = Host();
		if (host.Log && host.UserData)
			host.Log(host.UserData, level, message);
	}

	// 入口公共部分:宿主 ABI 不等值 = 显式返回 nullptr(干净拒绝,不猜布局)。
	template <typename MakePlugin>
	const WePlugin* Query(uint32_t hostAbiVersion, MakePlugin makePlugin)
	{
		if (hostAbiVersion != World::Plugins::WE_PLUGIN_ABI_VERSION)
			return nullptr;
		static const WePlugin plugin = makePlugin();
		return &plugin;
	}
}
