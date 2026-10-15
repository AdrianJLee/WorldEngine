#include "McpServer.h"
#include "World/Ai/CommandRegistry.h"
#include <sstream>

namespace World::Mcp
{
	using namespace World::Wui;

	McpServer::McpServer()
		: m_Bridge(std::make_shared<EngineBridge>())
	{
	}

	void McpServer::SetCurrentRole(const std::string& role)
	{
		m_CurrentRole = role;
	}

	std::string McpServer::HandleInitialize(const std::string& id, const JsonValue& /*params*/)
	{
		m_Initialized = true;

		JsonValue result;
		result.type = JsonValue::Type::Object;
		result.Object.push_back({ "protocolVersion", JsonValue::MakeString("2024-11-05") });

		JsonValue serverInfo;
		serverInfo.type = JsonValue::Type::Object;
		serverInfo.Object.push_back({ "name", JsonValue::MakeString("worldengine-mcp") });
		serverInfo.Object.push_back({ "version", JsonValue::MakeString("1.0.0") });
		result.Object.push_back({ "serverInfo", std::move(serverInfo) });

		JsonValue caps;
		caps.type = JsonValue::Type::Object;
		JsonValue toolsCap;
		toolsCap.type = JsonValue::Type::Object;
		toolsCap.Object.push_back({ "listChanged", JsonValue::MakeBool(true) });
		caps.Object.push_back({ "tools", std::move(toolsCap) });
		result.Object.push_back({ "capabilities", std::move(caps) });

		result.Object.push_back({ "instructions", JsonValue::MakeString(R"raw(You are an AI Game Producer and Department Director operating within WorldEngine.
Mandatory Workflow Guidelines:
1. Stages & Gates: Follow the 8-stage pipeline (Concept -> Fun Prototype -> Tech Prototype -> Vertical Slice -> Production -> Beta -> Polish -> Release). Check status via 'workflow.status' or 'gate.check'.
2. Taste & Direction: Check project taste profile via 'assistant.taste.get' and answer interview questions via 'assistant.interview.answer'.
3. Task Management: NEVER perform raw unorganized edits. Break game features into tasks using 'task.create' and update progress via 'task.update'. Log blockers with 'bug.create'.
4. Asset Pipeline: All texture, model, audio generation MUST be requested via 'gen.generate' which enforces style-lock and budget limits.
5. QA Verification: Always validate project health via 'studio.validate' and 'project.validate', and run headless simulation via 'playtest.run'.
6. Security: The engine source tree is read-only. Confine all game assets and scripts to the project root.)raw") });

		McpResponse resp;
		resp.id = id;
		resp.result = std::move(result);
		return resp.ToJson();
	}

	std::string McpServer::HandleToolsList(const std::string& id)
	{
		auto allDescriptors = Ai::CommandRegistry::Get().GetAllDescriptors();

		JsonValue result;
		result.type = JsonValue::Type::Object;

		JsonValue toolsArr;
		toolsArr.type = JsonValue::Type::Array;

		for (const auto& desc : allDescriptors)
		{
			// 基于角色的工具裁剪规则 (M6 契约)
			if (m_CurrentRole.empty())
			{
				// 未绑定会话角色时：仅暴露只读查询与 session.open
				if (desc.effect != Ai::CommandEffect::Read && desc.name != "session.open")
					continue;
			}
			else if (m_CurrentRole == "qa" || m_CurrentRole == "tester")
			{
				// 测试角色：仅保留验证、试玩、遥测与自证工具，裁剪写入脚本和资产的命令
				if (desc.effect == Ai::CommandEffect::Write &&
					desc.name.rfind("session.", 0) != 0 &&
					desc.name.rfind("workflow.", 0) != 0 &&
					desc.name.rfind("bug.", 0) != 0 &&
					desc.name.rfind("review.", 0) != 0)
				{
					continue;
				}
			}

			JsonValue tool;
			tool.type = JsonValue::Type::Object;
			tool.Object.push_back({ "name", JsonValue::MakeString(desc.name) });
			tool.Object.push_back({ "description", JsonValue::MakeString(desc.description) });

			// inputSchema
			std::string err;
			auto parsedSchema = JsonValue::Parse(desc.paramsSchema, &err);
			if (parsedSchema && parsedSchema->type == JsonValue::Type::Object)
			{
				tool.Object.push_back({ "inputSchema", std::move(*parsedSchema) });
			}
			else
			{
				JsonValue defaultSchema;
				defaultSchema.type = JsonValue::Type::Object;
				defaultSchema.Object.push_back({ "type", JsonValue::MakeString("object") });
				tool.Object.push_back({ "inputSchema", std::move(defaultSchema) });
			}

			toolsArr.Array.push_back(std::move(tool));
		}

		result.Object.push_back({ "tools", std::move(toolsArr) });

		McpResponse resp;
		resp.id = id;
		resp.result = std::move(result);
		return resp.ToJson();
	}

	std::string McpServer::HandleToolsCall(const std::string& id, const JsonValue& params, bool* outShouldNotifyListChanged)
	{
		const auto* nameVal = params.Find("name");
		if (!nameVal || nameVal->type != JsonValue::Type::String)
		{
			McpResponse resp;
			resp.id = id;
			resp.isError = true;
			resp.errorCode = -32602;
			resp.errorMessage = "Missing tool name in tools/call";
			return resp.ToJson();
		}

		const std::string cmd = nameVal->AsString();

		std::map<std::string, std::string> args;
		const auto* argsObj = params.Find("arguments");
		if (argsObj && argsObj->type == JsonValue::Type::Object)
		{
			for (const auto& kv : argsObj->Object)
			{
				if (kv.second.type == JsonValue::Type::String)
					args[kv.first] = kv.second.AsString();
				else if (kv.second.type == JsonValue::Type::Number)
					args[kv.first] = std::to_string(kv.second.AsNumber());
				else if (kv.second.type == JsonValue::Type::Bool)
					args[kv.first] = kv.second.AsBool() ? "true" : "false";
				else
					args[kv.first] = kv.second.Dump();
			}
		}

		std::string outResult;
		std::string outError;
		bool ok = false;
		if (m_Bridge)
		{
			ok = m_Bridge->Dispatch(cmd, args, outResult, outError);
		}
		else
		{
			ok = Ai::CommandRegistry::Get().Dispatch(cmd, args, outResult, outError);
		}

		// 如果执行了 session.open，切换活动角色并标记广播 list_changed
		if (cmd == "session.open" && ok)
		{
			if (args.count("role"))
			{
				m_CurrentRole = args.at("role");
				if (outShouldNotifyListChanged)
					*outShouldNotifyListChanged = true;
			}
		}

		JsonValue result;
		result.type = JsonValue::Type::Object;

		JsonValue contentArr;
		contentArr.type = JsonValue::Type::Array;

		JsonValue textItem;
		textItem.type = JsonValue::Type::Object;
		textItem.Object.push_back({ "type", JsonValue::MakeString("text") });
		textItem.Object.push_back({ "text", JsonValue::MakeString(ok ? outResult : ("Error: " + outError)) });
		contentArr.Array.push_back(std::move(textItem));

		result.Object.push_back({ "content", std::move(contentArr) });
		result.Object.push_back({ "isError", JsonValue::MakeBool(!ok) });

		McpResponse resp;
		resp.id = id;
		resp.result = std::move(result);
		return resp.ToJson();
	}

	std::string McpServer::HandleMessage(const std::string& jsonText, bool* outShouldNotifyListChanged)
	{
		std::string err;
		auto reqOpt = JsonValue::Parse(jsonText, &err);
		if (!reqOpt || reqOpt->type != JsonValue::Type::Object)
		{
			McpResponse resp;
			resp.id = "null";
			resp.isError = true;
			resp.errorCode = -32700;
			resp.errorMessage = "Parse error: " + err;
			return resp.ToJson();
		}

		const auto* idVal = reqOpt->Find("id");
		const std::string id = idVal ? (idVal->type == JsonValue::Type::String ? idVal->AsString() : std::to_string(static_cast<long long>(idVal->AsNumber()))) : "";

		const auto* methodVal = reqOpt->Find("method");
		const std::string method = methodVal ? methodVal->AsString() : "";

		const auto* paramsVal = reqOpt->Find("params");
		JsonValue emptyParams; emptyParams.type = JsonValue::Type::Object;
		const JsonValue& params = paramsVal ? *paramsVal : emptyParams;

		if (method == "initialize")
		{
			return HandleInitialize(id, params);
		}
		if (method == "notifications/initialized")
		{
			m_Initialized = true;
			return ""; // 通知无需回包
		}
		if (method == "tools/list")
		{
			return HandleToolsList(id);
		}
		if (method == "tools/call")
		{
			return HandleToolsCall(id, params, outShouldNotifyListChanged);
		}
		if (method == "ping")
		{
			McpResponse resp;
			resp.id = id;
			resp.result.type = JsonValue::Type::Object;
			return resp.ToJson();
		}

		// 未知方法
		McpResponse resp;
		resp.id = id;
		resp.isError = true;
		resp.errorCode = -32601;
		resp.errorMessage = "Method not found: " + method;
		return resp.ToJson();
	}

	void McpServer::RunStdioLoop(std::istream& in, std::ostream& out)
	{
		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty())
				continue;

			bool notify = false;
			const std::string resp = HandleMessage(line, &notify);
			if (!resp.empty())
			{
				out << resp << "\n";
				out.flush();
			}

			if (notify)
			{
				out << "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}\n";
				out.flush();
			}
		}
	}
}
