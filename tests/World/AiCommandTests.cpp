#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #expr); \
			std::exit(1); \
		} \
	} while (false)

int main()
{
	using namespace World::Ai;
	using namespace World::Wui;

	std::printf("Running WorldAiCommandTests...\n");

	auto& registry = CommandRegistry::Get();
	registry.ResetAll();

	// 1. 验证内置命令 ai.capabilities 默认存在
	{
		CHECK(registry.HasCommand("ai.capabilities"));
		const auto* desc = registry.GetDescriptor("ai.capabilities");
		CHECK(desc != nullptr);
		CHECK(desc->name == "ai.capabilities");
		CHECK(desc->effect == CommandEffect::Read);
		CHECK(desc->scope == CommandScope::Both);
		CHECK(desc->owner == "engine");
	}

	// 2. 验证执行 ai.capabilities 返回合法 JSON
	{
		std::string result;
		std::string error;
		std::map<std::string, std::string> args;
		const bool ok = registry.Dispatch("ai.capabilities", args, result, error);
		CHECK(ok);
		CHECK(error.empty());
		CHECK(!result.empty());

		std::string parseErr;
		auto jsonOpt = JsonValue::Parse(result, &parseErr);
		CHECK(jsonOpt.has_value());
		const auto* caps = jsonOpt->Find("capabilities");
		CHECK(caps != nullptr);
		CHECK(caps->type == JsonValue::Type::Array);
		CHECK(!caps->Array.empty());
	}

	// 3. 注册自定义命令并验证查找与派发
	{
		CommandDescriptor dummy;
		dummy.name = "test.ping";
		dummy.description = "Test ping command.";
		dummy.effect = CommandEffect::Read;
		dummy.scope = CommandScope::EditorOnly;
		dummy.owner = "plugin:test";
		dummy.paramsSchema = "{\"type\":\"object\",\"properties\":{\"msg\":{\"type\":\"string\"}}}";
		dummy.resultSchema = "{\"type\":\"string\"}";
		dummy.handler = [](const std::map<std::string, std::string>& args, std::string& res, std::string& /*err*/)
		{
			const auto it = args.find("msg");
			res = "pong:" + (it != args.end() ? it->second : "default");
			return true;
		};

		CHECK(registry.RegisterCommand(dummy));
		CHECK(registry.HasCommand("test.ping"));

		std::string result;
		std::string error;
		std::map<std::string, std::string> args = { {"msg", "hello_world"} };
		CHECK(registry.Dispatch("test.ping", args, result, error));
		CHECK(result == "pong:hello_world");
		CHECK(error.empty());

		// 验证未定义命令派发报错
		CHECK(!registry.Dispatch("unknown.cmd", args, result, error));
		CHECK(!error.empty());
	}

	// 4. 验证 ExportCapabilities 的范围与效果过滤
	{
		CommandDescriptor writeCmd;
		writeCmd.name = "test.write";
		writeCmd.description = "Test write command.";
		writeCmd.effect = CommandEffect::Write;
		writeCmd.scope = CommandScope::RuntimeOnly;
		writeCmd.owner = "engine";
		writeCmd.handler = [](const auto&, auto&, auto&) { return true; };
		CHECK(registry.RegisterCommand(writeCmd));

		// 过滤 scope = runtime
		std::string resRuntime = registry.ExportCapabilities("runtime", "");
		auto jsonRuntime = JsonValue::Parse(resRuntime, nullptr);
		CHECK(jsonRuntime.has_value());
		const auto* arrRuntime = jsonRuntime->Find("capabilities");
		CHECK(arrRuntime != nullptr);

		bool foundTestWrite = false;
		bool foundTestPing = false;
		for (const auto& item : arrRuntime->Array)
		{
			const auto* nameVal = item.Find("name");
			if (nameVal && nameVal->AsString() == "test.write") foundTestWrite = true;
			if (nameVal && nameVal->AsString() == "test.ping") foundTestPing = true;
		}
		CHECK(foundTestWrite);
		CHECK(!foundTestPing); // test.ping 是 EditorOnly，不应出现

		// 过滤 effect = write
		std::string resWrite = registry.ExportCapabilities("", "write");
		auto jsonWrite = JsonValue::Parse(resWrite, nullptr);
		CHECK(jsonWrite.has_value());
		const auto* arrWrite = jsonWrite->Find("capabilities");
		CHECK(arrWrite != nullptr);
		for (const auto& item : arrWrite->Array)
		{
			const auto* effVal = item.Find("effect");
			CHECK(effVal && effVal->AsString() == "write");
		}
	}

	// 5. 验证注销命令与清理插件命令
	{
		CHECK(registry.UnregisterCommand("test.write"));
		CHECK(!registry.HasCommand("test.write"));

		CHECK(registry.HasCommand("test.ping"));
		registry.ClearUserCommands(); // test.ping 是 plugin:test，应被清理
		CHECK(!registry.HasCommand("test.ping"));
		CHECK(registry.HasCommand("ai.capabilities")); // engine 内置保留
	}

	std::printf("WorldAiCommandTests PASSED.\n");
	return 0;
}
