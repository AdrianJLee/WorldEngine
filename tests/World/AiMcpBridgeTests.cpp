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

		// 验证 resources 能力宣告
		const auto* resCap = caps->Find("resources");
		CHECK(resCap != nullptr);
		CHECK(resCap->Find("listChanged")->AsBool() == true);
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

	// 8. 验证 MCP Resources 资源列表 (resources/list)
	{
		const std::string resListReq = "{\"jsonrpc\":\"2.0\",\"id\":\"res_list_1\",\"method\":\"resources/list\",\"params\":{}}";
		const std::string resListResp = server.HandleMessage(resListReq);
		auto json = JsonValue::Parse(resListResp, nullptr);
		CHECK(json.has_value());
		const auto* resArr = json->Find("result")->Find("resources");
		CHECK(resArr != nullptr && resArr->type == JsonValue::Type::Array);

		bool foundProj = false;
		bool foundSchemas = false;
		bool foundStubs = false;
		bool foundGroups = false;
		for (const auto& item : resArr->Array)
		{
			const std::string uri = item.Find("uri")->AsString();
			if (uri == "worldengine://project/info") foundProj = true;
			if (uri == "worldengine://schemas") foundSchemas = true;
			if (uri == "worldengine://api/stubs") foundStubs = true;
			if (uri == "worldengine://tool-groups") foundGroups = true;
		}
		CHECK(foundProj && foundSchemas && foundStubs && foundGroups);
	}

	// 9. 验证 MCP Resources 读取 (resources/read)
	{
		// 读 project/info
		const std::string readReq = "{\"jsonrpc\":\"2.0\",\"id\":\"res_read_1\",\"method\":\"resources/read\",\"params\":{\"uri\":\"worldengine://project/info\"}}";
		const std::string readResp = server.HandleMessage(readReq);
		auto json = JsonValue::Parse(readResp, nullptr);
		CHECK(json.has_value());
		const auto* contents = json->Find("result")->Find("contents");
		CHECK(contents != nullptr && !contents->Array.empty());
		const std::string text = contents->Array[0].Find("text")->AsString();
		CHECK(text.find("engineVersion") != std::string::npos);

		// 读 tool-groups
		const std::string readGrpReq = "{\"jsonrpc\":\"2.0\",\"id\":\"res_read_2\",\"method\":\"resources/read\",\"params\":{\"uri\":\"worldengine://tool-groups\"}}";
		const std::string readGrpResp = server.HandleMessage(readGrpReq);
		auto grpJson = JsonValue::Parse(readGrpResp, nullptr);
		CHECK(grpJson.has_value());
		const auto* grpContents = grpJson->Find("result")->Find("contents");
		CHECK(grpContents != nullptr && !grpContents->Array.empty());
		const std::string grpText = grpContents->Array[0].Find("text")->AsString();
		CHECK(grpText.find("core") != std::string::npos);

		// 读非法 URI 返回错误
		const std::string readErrReq = "{\"jsonrpc\":\"2.0\",\"id\":\"res_read_err\",\"method\":\"resources/read\",\"params\":{\"uri\":\"worldengine://invalid\"}}";
		const std::string readErrResp = server.HandleMessage(readErrReq);
		auto errJson = JsonValue::Parse(readErrResp, nullptr);
		CHECK(errJson.has_value());
		CHECK(errJson->Find("error") != nullptr);
	}

	// 10. 验证 Tool Groups 动态激活与裁剪 (manage_tools)
	{
		server.SetCurrentRole("producer");
		server.ResetToolGroups(); // 默认 core, scene, asset, playtest

		// 默认情况下 vfx 组未激活，tools/list 不应包含 vfx.list
		{
			const std::string listReq = "{\"jsonrpc\":\"2.0\",\"id\":\"grp_list_1\",\"method\":\"tools/list\",\"params\":{}}";
			const std::string listResp = server.HandleMessage(listReq);
			auto json = JsonValue::Parse(listResp, nullptr);
			CHECK(json.has_value());
			const auto* tools = json->Find("result")->Find("tools");
			bool foundVfx = false;
			for (const auto& t : tools->Array)
			{
				if (t.Find("name")->AsString() == "vfx.list") foundVfx = true;
			}
			CHECK(!foundVfx);
		}

		// 调用 manage_tools 激活 vfx 组
		{
			bool notify = false;
			const std::string actReq = "{\"jsonrpc\":\"2.0\",\"id\":\"act_1\",\"method\":\"tools/call\",\"params\":{\"name\":\"manage_tools\",\"arguments\":{\"action\":\"activate\",\"group\":\"vfx\"}}}";
			const std::string actResp = server.HandleMessage(actReq, &notify);
			CHECK(notify == true); // 触发 list_changed
			auto json = JsonValue::Parse(actResp, nullptr);
			CHECK(json.has_value());
			CHECK(!json->Find("result")->Find("isError")->AsBool());
		}

		// 激活后，tools/list 应包含 vfx.list
		{
			const std::string listReq = "{\"jsonrpc\":\"2.0\",\"id\":\"grp_list_2\",\"method\":\"tools/list\",\"params\":{}}";
			const std::string listResp = server.HandleMessage(listReq);
			auto json = JsonValue::Parse(listResp, nullptr);
			CHECK(json.has_value());
			const auto* tools = json->Find("result")->Find("tools");
			bool foundVfx = false;
			for (const auto& t : tools->Array)
			{
				if (t.Find("name")->AsString() == "vfx.list") foundVfx = true;
			}
			CHECK(foundVfx);
		}

		// 尝试停用 core 组应被拦截
		{
			bool notify = false;
			const std::string deactCoreReq = "{\"jsonrpc\":\"2.0\",\"id\":\"act_err\",\"method\":\"tools/call\",\"params\":{\"name\":\"manage_tools\",\"arguments\":{\"action\":\"deactivate\",\"group\":\"core\"}}}";
			const std::string deactCoreResp = server.HandleMessage(deactCoreReq, &notify);
			auto json = JsonValue::Parse(deactCoreResp, nullptr);
			CHECK(json.has_value());
			CHECK(json->Find("result")->Find("isError")->AsBool());
		}
	}

	// 11. 验证批量执行能力 (batch_execute)
	{
		server.SetCurrentRole("producer");
		// 构造一个批量执行两条只读指令的请求
		const std::string batchReq = R"raw({
			"jsonrpc": "2.0",
			"id": "batch_1",
			"method": "tools/call",
			"params": {
				"name": "batch_execute",
				"arguments": {
					"commands": [
						{ "tool": "project.validate", "params": {} },
						{ "tool": "schema.list", "params": {} }
					],
					"fail_fast": false
				}
			}
		})raw";

		bool notify = false;
		const std::string batchResp = server.HandleMessage(batchReq, &notify);
		CHECK(!batchResp.empty());

		auto json = JsonValue::Parse(batchResp, nullptr);
		CHECK(json.has_value());
		const auto* res = json->Find("result");
		CHECK(res != nullptr);
		CHECK(!res->Find("isError")->AsBool());

		const auto* contentArr = res->Find("content");
		CHECK(contentArr && !contentArr->Array.empty());
		const std::string payload = contentArr->Array[0].Find("text")->AsString();

		auto batchData = JsonValue::Parse(payload, nullptr);
		CHECK(batchData.has_value());
		CHECK(batchData->Find("total")->AsNumber() == 2);
		CHECK(batchData->Find("executed")->AsNumber() == 2);
		CHECK(batchData->Find("succeeded")->AsNumber() == 2);
		CHECK(batchData->Find("failed")->AsNumber() == 0);
	}

	// 12. 验证 batch_execute 的嵌套拦截与 fail_fast
	{
		server.SetCurrentRole("producer");
		// 构造包含嵌套 batch_execute 的请求
		const std::string nestedReq = R"raw({
			"jsonrpc": "2.0",
			"id": "batch_nested",
			"method": "tools/call",
			"params": {
				"name": "batch_execute",
				"arguments": {
					"commands": [
						{ "tool": "batch_execute", "params": {} }
					]
				}
			}
		})raw";

		const std::string nestedResp = server.HandleMessage(nestedReq);
		auto json = JsonValue::Parse(nestedResp, nullptr);
		CHECK(json.has_value());
		const auto* contentArr = json->Find("result")->Find("content");
		CHECK(contentArr && !contentArr->Array.empty());
		const std::string text = contentArr->Array[0].Find("text")->AsString();
		CHECK(text.find("Nested batch_execute is not permitted") != std::string::npos);
	}

	std::printf("WorldAiMcpBridgeTests PASSED.\n"); fflush(stdout);
	std::quick_exit(0);
	return 0;
}
