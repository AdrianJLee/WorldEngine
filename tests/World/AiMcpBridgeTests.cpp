#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"

// 直接包含 McpServer 实现文件与头文件以方便纯单元测试
#include "../../Launcher/WeMcp/src/McpServer.h"
#include "../../Launcher/WeMcp/src/McpProtocol.h"

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
	using namespace World::Mcp;
	using namespace World::Wui;

	std::printf("Running WorldAiMcpBridgeTests...\n");

	McpServer server;

	// 1. 验证 initialize 握手与能力协商
	{
		const std::string initReq = "{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"initialize\",\"params\":{}}";
		const std::string initResp = server.HandleMessage(initReq);
		CHECK(!initResp.empty());

		auto json = JsonValue::Parse(initResp, nullptr);
		CHECK(json.has_value());
		const auto* res = json->Find("result");
		CHECK(res != nullptr);
		const auto* serverInfo = res->Find("serverInfo");
		CHECK(serverInfo != nullptr);
		CHECK(serverInfo->Find("name")->AsString() == "worldengine-mcp");

		const auto* caps = res->Find("capabilities");
		CHECK(caps != nullptr);
		const auto* toolsCap = caps->Find("tools");
		CHECK(toolsCap != nullptr);
		CHECK(toolsCap->Find("listChanged")->AsBool() == true);
	}

	// 2. 验证未指定角色时的工具裁剪 (仅暴露只读查询与 session.open)
	{
		server.SetCurrentRole("");
		const std::string listReq = "{\"jsonrpc\":\"2.0\",\"id\":\"2\",\"method\":\"tools/list\",\"params\":{}}";
		const std::string listResp = server.HandleMessage(listReq);
		auto json = JsonValue::Parse(listResp, nullptr);
		CHECK(json.has_value());
		const auto* tools = json->Find("result")->Find("tools");
		CHECK(tools != nullptr && tools->type == JsonValue::Type::Array);

		bool foundSessionOpen = false;
		bool foundPlaytestRun = false;
		for (const auto& t : tools->Array)
		{
			const std::string name = t.Find("name")->AsString();
			if (name == "session.open") foundSessionOpen = true;
			if (name == "playtest.run") foundPlaytestRun = true;
		}
		CHECK(foundSessionOpen);
		CHECK(!foundPlaytestRun); // 未认证会话时不暴露 playtest.run (属于 Exec)
	}

	// 3. 验证通过 tools/call 执行 session.open 并触发 list_changed
	{
		const std::string callReq = "{\"jsonrpc\":\"2.0\",\"id\":\"3\",\"method\":\"tools/call\",\"params\":{\"name\":\"session.open\",\"arguments\":{\"role\":\"qa\",\"task\":\"TEST-QA\"}}}";
		bool notifyChanged = false;
		const std::string callResp = server.HandleMessage(callReq, &notifyChanged);
		CHECK(!callResp.empty());
		CHECK(notifyChanged == true);
		CHECK(server.GetCurrentRole() == "qa");

		auto json = JsonValue::Parse(callResp, nullptr);
		CHECK(json.has_value());
		const auto* res = json->Find("result");
		CHECK(res != nullptr);
		const auto* isErr = res->Find("isError");
		CHECK(isErr && !isErr->AsBool());
	}

	// 4. 验证角色为 QA 时的工具动态裁剪
	{
		const std::string listReq = "{\"jsonrpc\":\"2.0\",\"id\":\"4\",\"method\":\"tools/list\",\"params\":{}}";
		const std::string listResp = server.HandleMessage(listReq);
		auto json = JsonValue::Parse(listResp, nullptr);
		CHECK(json.has_value());
		const auto* tools = json->Find("result")->Find("tools");
		CHECK(tools != nullptr);

		bool foundPlaytest = false;
		bool foundProjectValidate = false;
		bool foundChangesetBegin = false;
		for (const auto& t : tools->Array)
		{
			const std::string name = t.Find("name")->AsString();
			if (name == "playtest.run") foundPlaytest = true;
			if (name == "project.validate") foundProjectValidate = true;
			if (name == "changeset.begin") foundChangesetBegin = true;
		}
		CHECK(foundPlaytest);
		CHECK(foundProjectValidate);
		CHECK(!foundChangesetBegin); // QA 角色不暴露写代码变更集
	}

	// 5. 验证执行 ping
	{
		const std::string pingReq = "{\"jsonrpc\":\"2.0\",\"id\":\"5\",\"method\":\"ping\",\"params\":{}}";
		const std::string pingResp = server.HandleMessage(pingReq);
		auto json = JsonValue::Parse(pingResp, nullptr);
		CHECK(json.has_value());
		CHECK(json->Find("result") != nullptr);
		}


	// 6. 验证 JSON-RPC 2.0 整型 ID 保真 (Codex / rmcp 客户端发送 "id": 1 时必须返回数字 1 而非字符串 "1")
	{
		const std::string intIdReq = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";
		const std::string intIdResp = server.HandleMessage(intIdReq);
		auto json = JsonValue::Parse(intIdResp, nullptr);
		CHECK(json.has_value());
		const auto* idNode = json->Find("id");
		CHECK(idNode != nullptr && idNode->type == JsonValue::Type::Number);
		CHECK(static_cast<int>(idNode->AsNumber()) == 1);
	}

	// 7. 验证所有暴露工具的 inputSchema 均包含 type="object" 与 properties
	{
		server.SetCurrentRole("producer");
		const std::string listReq = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}";
		const std::string listResp = server.HandleMessage(listReq);
		auto json = JsonValue::Parse(listResp, nullptr);
		CHECK(json.has_value());
		const auto* tools = json->Find("result")->Find("tools");
		CHECK(tools != nullptr && !tools->Array.empty());
		bool foundAssetImport = false;
		for (const auto& t : tools->Array)
		{
			const auto* schema = t.Find("inputSchema");
			CHECK(schema != nullptr && schema->type == JsonValue::Type::Object);
			const auto* typeField = schema->Find("type");
			CHECK(typeField != nullptr && typeField->AsString() == "object");
			CHECK(schema->Find("properties") != nullptr);
			if (t.Find("name")->AsString() == "asset.import")
				foundAssetImport = true;
		}
		CHECK(foundAssetImport);
	}

	std::printf("WorldAiMcpBridgeTests PASSED.\n"); fflush(stdout);
	std::quick_exit(0);
	return 0;
}
