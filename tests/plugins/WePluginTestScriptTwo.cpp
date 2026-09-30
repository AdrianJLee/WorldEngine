// PLUG-T4 负例/边角:第二个脚本库插件。
//
// 覆盖:
//   * 与 test.scriptlib **共享命名空间** hello(注册 hello.two),外加独立命名空间 zeta.mul ——
//     单测据此断言存根顺序是 (插件 id, 函数名) 而不是装载顺序(本插件目录名排在前,先加载);
//   * 导出 `scripttwo.unregister`:尝试注销**别的插件**拥有的 hello.ping —— 宿主必须拒绝
//     (归属保护,false + 警告),本插件自身不受影响;
//   * Unregister **故意违约**(不注销自己注册的函数):单测据此断言卸载/析构的兜底回收
//     会把 hello.two / zeta.mul 强删 + 记 WARN,且之后可以干净重载。
// 只依赖公共 ABI 头(不链接 World)。
#include "PluginTestSupport.h"

#include <cstdint>
#include <string>

using namespace World::Plugins;

namespace
{
	constexpr const char* kTwoName = "hello.two";
	constexpr const char* kMulName = "zeta.mul";
	constexpr const char* kForeignName = "hello.ping";

	void Two(void*, const WeScriptCallApi* call)
	{
		if (!call || !call->PushNumber) return;
		call->PushNumber(call->UserData, 2.0);
	}

	void Mul(void*, const WeScriptCallApi* call)
	{
		if (!call || !call->GetArgNumber || !call->PushNumber) return;
		double left = 0.0;
		double right = 0.0;
		if (!call->GetArgNumber(call->UserData, 0, &left)
			|| !call->GetArgNumber(call->UserData, 1, &right))
		{
			if (call->PushNil) call->PushNil(call->UserData);
			return;
		}
		call->PushNumber(call->UserData, left * right);
	}

	// 导出入口:尝试注销别的插件拥有的函数 —— 期望被宿主拒绝(返回 true = 拒绝到位)。
	bool UnregisterForeign()
	{
		const WeHostApi& host = WeTestPlugin::Host();
		if (!host.UserData || !host.UnregisterScriptFunction)
			return false;
		const bool accepted = host.UnregisterScriptFunction(host.UserData, kForeignName);
		WeTestPlugin::Log(accepted ? "scripttwo-cross-owner-accepted" : "scripttwo-cross-owner-rejected",
			accepted ? WePluginLogError : WePluginLogInfo);
		return !accepted;
	}

	const WePluginExport kExports[] =
	{
		{ "scripttwo.unregister-foreign", 1, reinterpret_cast<void*>(&UnregisterForeign) },
	};

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host) || !WeTestPlugin::HostApiHasScriptFunctionSurface(host))
		{
			WeTestPlugin::Log("host script function surface missing; rejected", WePluginLogError);
			return false;
		}
		WeTestPlugin::Host() = host;

		WeScriptFunctionDesc two;
		two.Name = kTwoName;
		two.Signature = "(): number";
		two.Doc = "Returns the constant 2.";
		two.Callback = &Two;
		const bool twoOk = host.RegisterScriptFunction(host.UserData, &two);

		WeScriptFunctionDesc mul;
		mul.Name = kMulName;
		mul.Signature = "(left: number, right: number): number";
		mul.Doc = "Multiplies two numbers.";
		mul.Callback = &Mul;
		const bool mulOk = host.RegisterScriptFunction(host.UserData, &mul);

		WeTestPlugin::Log((twoOk && mulOk) ? "scripttwo-registered" : "scripttwo-register-failed",
			(twoOk && mulOk) ? WePluginLogInfo : WePluginLogError);
		return twoOk && mulOk;
	}

	void Unregister(World::WorldContext&)
	{
		// 违约:故意不注销 hello.two / zeta.mul —— 钉卸载/析构的兜底回收路径。
		WeTestPlugin::Log("scripttwo-leaves-its-functions-registered (contract violation test)");
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.scripttwo";
		plugin.Name = "Test Script Library Two";
		plugin.Version = "1.0.0";
		plugin.Exports = kExports;
		plugin.ExportCount = 1;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_TEST_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	return WeTestPlugin::Query(hostAbiVersion, &MakePlugin);
}
