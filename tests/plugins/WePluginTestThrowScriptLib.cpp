// PLUG-T4 负例:注册了脚本函数之后 Register 抛异常 —— 契约违约的回滚路径。
//
// 单测断言:
//   * 条目被拒绝(Status::Rejected + 可读诊断);
//   * best-effort Unregister 被调用(插件日志 "unregister-after-script-throw");
//   * 宿主兜底回收把 throwlib.ping 强删并记 WARN("was not unregistered by the plugin"),
//     账本与 VM 全局表都不留成员;
//   * 之后可以干净重载(不留脏账)。
// 只依赖公共 ABI 头(不链接 World)。
#include "PluginTestSupport.h"

#include <cstdint>
#include <stdexcept>

using namespace World::Plugins;

namespace
{
	constexpr const char* kFunctionName = "throwlib.ping";

	void Ping(void*, const WeScriptCallApi* call)
	{
		if (!call || !call->PushNumber) return;
		call->PushNumber(call->UserData, 1.0);
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host) || !WeTestPlugin::HostApiHasScriptFunctionSurface(host))
		{
			WeTestPlugin::Log("host script function surface missing; rejected", WePluginLogError);
			return false;
		}
		WeTestPlugin::Host() = host;

		WeScriptFunctionDesc ping;
		ping.Name = kFunctionName;
		ping.Signature = "(): number";
		ping.Doc = "Registered before the throwing plugin fails.";
		ping.Callback = &Ping;
		const bool registered = host.RegisterScriptFunction(host.UserData, &ping);
		WeTestPlugin::Log(registered ? "throw-scriptlib-registered" : "throw-scriptlib-register-failed",
			registered ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log("throw-scriptlib throwing after registering");
		throw std::runtime_error("script library registration threw");
	}

	void Unregister(World::WorldContext&)
	{
		// 违约(回滚测试):不注销 —— 宿主兜底回收必须强删 + 记 WARN。
		WeTestPlugin::Log("unregister-after-script-throw");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.throwscript";
		plugin.Name = "Test Throw Script Library";
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
