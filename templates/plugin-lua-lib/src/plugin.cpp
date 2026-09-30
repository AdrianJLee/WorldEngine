// {{PluginName}} — Lua 库插件(Lua Library Plugin 模板,{{PluginScope}} 形态)。
//
// 真实注册:经 WeHostApi::RegisterScriptFunction 注册一个全局 Luau 函数
// `<命名空间>.ping(name?)` —— 命名空间 = 插件 id 的最后一段(非法字符替换为 '_',
// 数字开头加 "plugin_" 前缀),脚本里写 `<命名空间>.ping("world")`。
// Unregister 与注册成对注销(幂等);只依赖公共 ABI 头(World/Plugins/WePluginApi.h),
// 不链接 World。
#include "World/Plugins/WePluginApi.h"

#include <cstddef>
#include <cstdint>
#include <string>

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

	// Unregister 没有 host 参数 —— 注册期自己记住宿主表(值拷贝 = 一组函数指针)。
	WeHostApi g_Host;
	std::string g_FunctionName;   // "<命名空间>.ping"

	void Log(const char* message, int level = WePluginLogInfo)
	{
		if (g_Host.Log && g_Host.UserData)
			g_Host.Log(g_Host.UserData, level, message);
	}

	bool HostApiIsCompatible(const WeHostApi& host)
	{
		// 判据 = 宿主表覆盖到**最后一个**追加字段(offsetof + sizeof);旧宿主在这里干净拒绝。
		return host.StructSize >= offsetof(WeHostApi, UnregisterScriptFunction)
				+ sizeof(host.UnregisterScriptFunction)
			&& host.AbiVersion == WE_PLUGIN_ABI_VERSION
			&& host.Log != nullptr
			&& host.RegisterScriptFunction != nullptr
			&& host.UnregisterScriptFunction != nullptr;
	}

	bool IsIdentifierChar(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '_';
	}

	// 插件 id 的最后一段 → 合法 Lua 标识符(反域名里的 '-' 等字符替换为 '_')。
	std::string LuaNamespace()
	{
		std::string text = kPluginId;
		const std::size_t dot = text.find_last_of('.');
		if (dot != std::string::npos)
			text = text.substr(dot + 1);
		for (char& c : text)
			if (!IsIdentifierChar(c)) c = '_';
		if (text.empty() || (text[0] >= '0' && text[0] <= '9'))
			text = "plugin_" + text;
		return text;
	}

	// 注册的脚本函数:可选参数 name(string),返回问候语(string)。
	void Ping(void* userData, const WeScriptCallApi* call)
	{
		if (!call || !call->PushString) return;
		const char* who = nullptr;
		if (call->GetArgString && call->ArgCount > 0)
			call->GetArgString(call->UserData, 0, &who);
		const std::string from = static_cast<const char*>(userData);
		const std::string message = from + ": pong"
			+ ((who && who[0]) ? std::string(" from ") + who : std::string());
		call->PushString(call->UserData, message.c_str());
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!HostApiIsCompatible(host))
		{
			// 宿主表还没覆盖脚本函数注册面(旧引擎)→ 干净拒绝,不按旧布局解释。
			if (host.Log && host.UserData)
				host.Log(host.UserData, WePluginLogError,
					"host script function surface missing; plugin rejected");
			return false;
		}
		g_Host = host;

		g_FunctionName = LuaNamespace() + ".ping";
		WeScriptFunctionDesc desc;
		desc.Name = g_FunctionName.c_str();
		desc.Signature = "(name: string?): string";
		desc.Doc = "Returns a greeting from this generated Lua library plugin.";
		desc.Callback = &Ping;
		desc.UserData = const_cast<char*>(kPluginName);
		if (!host.RegisterScriptFunction(host.UserData, &desc))
		{
			Log("script function registration rejected", WePluginLogError);
			g_FunctionName.clear();
			return false;
		}
		Log("lua library registered");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		if (!g_FunctionName.empty() && g_Host.UnregisterScriptFunction && g_Host.UserData)
			g_Host.UnregisterScriptFunction(g_Host.UserData, g_FunctionName.c_str());   // 幂等
		g_FunctionName.clear();
		Log("lua library unregistered");
	}
}

// 入口:宿主 ABI 不等值 = 干净拒绝(不按旧布局解释)。
WE_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	if (hostAbiVersion != WE_PLUGIN_ABI_VERSION)
		return nullptr;
	static const WePlugin plugin = []
	{
		WePlugin value;
		value.Id = kPluginId;
		value.Name = kPluginName;
		value.Version = "1.0.0";
		value.MinEngineVersion = ">=2.0";
		static const char* const provides[] = { "script.library" };
		value.Provides = provides;
		value.ProvidesCount = 1;
		value.Register = &Register;
		value.Unregister = &Unregister;
		return value;
	}();
	return &plugin;
}
