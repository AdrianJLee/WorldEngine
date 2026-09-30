// PLUG-T4 正例:脚本函数库注册(WeHostApi::RegisterScriptFunction / UnregisterScriptFunction)。
//
// 覆盖(插件侧证据全部经 host.Log 回传,由 World.Plugins 单测取回):
//   * 坏描述干净拒绝:StructSize 不足 / ABI 不符 / 空名 / 名字缺命名空间 / 签名非法 / 无回调;
//   * 重复 name(本插件内)= 拒绝(不覆盖);
//   * 真注册三个函数:`hello.ping`(number+number→number)、`hello.echo`(string→string)、
//     `hello.boom`(抛 C++ 异常 —— 单测用它钉"异常不跨 ABI 边界,转成 Lua error");
//   * 导出 `scriptlib.unregister`:按需注销 `hello.echo` 两次(幂等),单测据此断言注销路径;
//   * Unregister 与注册成对(幂等),卸载后账本与 VM 全局表都不留成员。
// 只依赖公共 ABI 头(不链接 World)。
#include "PluginTestSupport.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

using namespace World::Plugins;

namespace
{
	constexpr const char* kPingName = "hello.ping";
	constexpr const char* kEchoName = "hello.echo";
	constexpr const char* kBoomName = "hello.boom";

	void Ping(void* userData, const WeScriptCallApi* call)
	{
		(void)userData;
		if (!call || !call->GetArgNumber || !call->PushNumber) return;
		double left = 0.0;
		double right = 0.0;
		if (!call->GetArgNumber(call->UserData, 0, &left)
			|| !call->GetArgNumber(call->UserData, 1, &right))
		{
			if (call->PushNil) call->PushNil(call->UserData);
			return;
		}
		call->PushNumber(call->UserData, left + right);
	}

	void Echo(void* userData, const WeScriptCallApi* call)
	{
		(void)userData;
		if (!call || !call->GetArgString || !call->PushString) return;
		const char* text = nullptr;
		if (!call->GetArgString(call->UserData, 0, &text))
		{
			if (call->PushNil) call->PushNil(call->UserData);
			return;
		}
		const std::string echoed = std::string(text ? text : "") + "!";
		call->PushString(call->UserData, echoed.c_str());
	}

	void Boom(void*, const WeScriptCallApi*)
	{
		throw std::runtime_error("boom from the script library plugin");
	}

	bool RegisterBadDescriptors(const WeHostApi& host)
	{
		// ① StructSize 小于宿主读取所需的前缀(读不到 Callback 字段)→ 干净拒绝。
		WeScriptFunctionDesc smallSize;
		smallSize.StructSize = offsetof(WeScriptFunctionDesc, Callback);
		smallSize.Name = "hello.small";
		smallSize.Callback = &Ping;
		const bool sizeOk = host.RegisterScriptFunction(host.UserData, &smallSize);
		WeTestPlugin::Log(sizeOk ? "scriptlib-size-accepted" : "scriptlib-size-rejected",
			sizeOk ? WePluginLogError : WePluginLogInfo);

		// ② ABI 不等值 → 干净拒绝。
		WeScriptFunctionDesc badAbi;
		badAbi.AbiVersion = WE_PLUGIN_ABI_VERSION + 1;
		badAbi.Name = "hello.badabi";
		badAbi.Callback = &Ping;
		const bool abiOk = host.RegisterScriptFunction(host.UserData, &badAbi);
		WeTestPlugin::Log(abiOk ? "scriptlib-abi-accepted" : "scriptlib-abi-rejected",
			abiOk ? WePluginLogError : WePluginLogInfo);

		// ③ 空名 → 干净拒绝。
		WeScriptFunctionDesc emptyName;
		emptyName.Name = "";
		emptyName.Callback = &Ping;
		const bool emptyOk = host.RegisterScriptFunction(host.UserData, &emptyName);
		WeTestPlugin::Log(emptyOk ? "scriptlib-empty-accepted" : "scriptlib-empty-rejected",
			emptyOk ? WePluginLogError : WePluginLogInfo);

		// ④ 名字缺命名空间(没有点)→ 干净拒绝。
		WeScriptFunctionDesc noNamespace;
		noNamespace.Name = "nodot";
		noNamespace.Callback = &Ping;
		const bool namespaceOk = host.RegisterScriptFunction(host.UserData, &noNamespace);
		WeTestPlugin::Log(namespaceOk ? "scriptlib-nonamespace-accepted" : "scriptlib-nonamespace-rejected",
			namespaceOk ? WePluginLogError : WePluginLogInfo);

		// ⑤ 签名非法(缺右括号)→ 干净拒绝。
		WeScriptFunctionDesc badSignature;
		badSignature.Name = "hello.badsig";
		badSignature.Signature = "(value: number";
		badSignature.Callback = &Ping;
		const bool signatureOk = host.RegisterScriptFunction(host.UserData, &badSignature);
		WeTestPlugin::Log(signatureOk ? "scriptlib-badsig-accepted" : "scriptlib-badsig-rejected",
			signatureOk ? WePluginLogError : WePluginLogInfo);

		// ⑥ 没有回调 → 干净拒绝。
		WeScriptFunctionDesc noCallback;
		noCallback.Name = "hello.nocallback";
		const bool callbackOk = host.RegisterScriptFunction(host.UserData, &noCallback);
		WeTestPlugin::Log(callbackOk ? "scriptlib-nocallback-accepted" : "scriptlib-nocallback-rejected",
			callbackOk ? WePluginLogError : WePluginLogInfo);

		return !sizeOk && !abiOk && !emptyOk && !namespaceOk && !signatureOk && !callbackOk;
	}

	// 导出入口:按需注销一个函数(单测经 LookupExport 调用);幂等。
	bool UnregisterByName(const char* rawName)
	{
		const WeHostApi& host = WeTestPlugin::Host();
		if (!rawName || !host.UserData || !host.UnregisterScriptFunction)
			return false;
		const bool first = host.UnregisterScriptFunction(host.UserData, rawName);
		const bool second = host.UnregisterScriptFunction(host.UserData, rawName);
		const std::string message = std::string("scriptlib.unregister ") + rawName + " -> "
			+ ((first && second) ? "idempotent-ok" : "failed");
		WeTestPlugin::Log(message.c_str(), (first && second) ? WePluginLogInfo : WePluginLogError);
		return first && second;
	}

	const WePluginExport kExports[] =
	{
		{ "scriptlib.unregister", 1, reinterpret_cast<void*>(&UnregisterByName) },
	};

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		if (!WeTestPlugin::HostApiIsCompatible(host) || !WeTestPlugin::HostApiHasScriptFunctionSurface(host))
		{
			WeTestPlugin::Log("host script function surface missing; rejected", WePluginLogError);
			return false;
		}
		WeTestPlugin::Host() = host;

		if (!RegisterBadDescriptors(host))
		{
			WeTestPlugin::Log("bad script function descriptors were not rejected", WePluginLogError);
			return false;
		}

		WeScriptFunctionDesc ping;
		ping.Name = kPingName;
		ping.Signature = "(left: number, right: number): number";
		ping.Doc = "Adds two numbers and returns the sum.";
		ping.Callback = &Ping;
		const bool pingOk = host.RegisterScriptFunction(host.UserData, &ping);
		// 重复 name(同一插件内)= 拒绝(不覆盖)。
		const bool duplicate = host.RegisterScriptFunction(host.UserData, &ping);
		WeTestPlugin::Log(pingOk ? "scriptlib-ping-registered" : "scriptlib-ping-register-failed",
			pingOk ? WePluginLogInfo : WePluginLogError);
		WeTestPlugin::Log(duplicate ? "scriptlib-duplicate-accepted" : "scriptlib-duplicate-rejected",
			WePluginLogInfo);

		WeScriptFunctionDesc echo;
		echo.Name = kEchoName;
		echo.Signature = "(text: string): string";
		echo.Doc = "Returns the input text with a trailing '!'.";
		echo.Callback = &Echo;
		const bool echoOk = host.RegisterScriptFunction(host.UserData, &echo);
		WeTestPlugin::Log(echoOk ? "scriptlib-echo-registered" : "scriptlib-echo-register-failed",
			echoOk ? WePluginLogInfo : WePluginLogError);

		WeScriptFunctionDesc boom;
		boom.Name = kBoomName;
		boom.Signature = "()";
		boom.Doc = "Always raises; used to pin the ABI exception contract.";
		boom.Callback = &Boom;
		const bool boomOk = host.RegisterScriptFunction(host.UserData, &boom);
		WeTestPlugin::Log(boomOk ? "scriptlib-boom-registered" : "scriptlib-boom-register-failed",
			boomOk ? WePluginLogInfo : WePluginLogError);

		if (!pingOk || duplicate || !echoOk || !boomOk)
			return false;
		WeTestPlugin::Log("scriptlib-registered");
		return true;
	}

	void Unregister(World::WorldContext&)
	{
		const WeHostApi& host = WeTestPlugin::Host();
		if (!host.UserData || !host.UnregisterScriptFunction)
		{
			WeTestPlugin::Log("script unregister surface missing", WePluginLogError);
			return;
		}
		// 按契约成对注销(即使已经被导出回调注销过 —— 幂等)。
		const bool first = host.UnregisterScriptFunction(host.UserData, kPingName);
		const bool second = host.UnregisterScriptFunction(host.UserData, kEchoName);
		const bool third = host.UnregisterScriptFunction(host.UserData, kBoomName);
		const bool idempotent = host.UnregisterScriptFunction(host.UserData, kPingName);
		const bool ok = first && second && third && idempotent;
		WeTestPlugin::Log(ok ? "scriptlib-unregistered idempotent" : "scriptlib-unregister-failed",
			ok ? WePluginLogInfo : WePluginLogError);
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = "test.scriptlib";
		plugin.Name = "Test Script Library";
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
