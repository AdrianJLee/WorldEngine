#include "McpServer.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Agent/WeAgentCore.h"
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
1. Session & Role First: Always open a session via 'session.open' with your assigned role before executing write/exec tools.
2. Stages & Gates: Follow the 8-stage pipeline. Check status via 'workflow.status' or 'gate.check'.
3. Taste & Direction: Check project taste profile via 'assistant.taste.get' and answer interview questions via 'assistant.interview.answer'.
4. Task Management: Break game features into tasks using 'task.create', update via 'task.update', and submit independent review via 'review.submit' (self-review is forbidden). Log blockers with 'bug.create'.
5. Asset Pipeline: All texture, model, audio generation MUST be requested via 'gen.generate' which enforces style-lock and budget limits.
6. QA Verification: Always validate project health via 'studio.validate' and 'project.validate', and run headless simulation via 'playtest.run'.
7. Security: The engine source tree is read-only. Confine all game assets and scripts to the project root.)raw") });

		McpResponse resp;
		resp.id = id;
		resp.result = std::move(result);
		return resp.ToJson();
	}

	std::string McpServer::HandleToolsList(const std::string& id)
	{
		auto allDescriptors = Ai::CommandRegistry::Get().GetAllDescriptors();

		// 若已连接远端宿主，同步拉取远端 ai.capabilities 合并编辑器专属命令
		if (m_Bridge && m_Bridge->IsConnectedToRemote())
		{
			std::string capRes, capErr;
			if (m_Bridge->Dispatch("ai.capabilities", {}, capRes, capErr))
			{
				auto parsedCaps = JsonValue::Parse(capRes, nullptr);
				if (parsedCaps && parsedCaps->type == JsonValue::Type::Object)
				{
					const auto* arr = parsedCaps->Find("capabilities");
					if (arr && arr->type == JsonValue::Type::Array)
					{
						for (const auto& item : arr->Array)
						{
							const auto* nVal = item.Find("name");
							if (!nVal) continue;
							const std::string cName = nVal->AsString();
							if (Ai::CommandRegistry::Get().HasCommand(cName))
								continue;

							Ai::CommandDescriptor d;
							d.name = cName;
							if (const auto* descVal = item.Find("description")) d.description = descVal->AsString();
							if (const auto* effVal = item.Find("effect")) d.effect = Ai::StringToCommandEffect(effVal->AsString());
							if (const auto* scpVal = item.Find("scope")) d.scope = Ai::StringToCommandScope(scpVal->AsString());
							if (const auto* pSchema = item.Find("params_schema")) d.paramsSchema = pSchema->Dump();
							allDescriptors.push_back(std::move(d));
						}
					}
				}
			}
		}

		auto& core = Agent::WeAgentCore::Get();
		const auto currentStage = core.GetCurrentStage();
		const auto policy = core.GetPolicy();
		const bool readOnlyFallback = (m_Bridge && m_Bridge->IsReadOnlyFallback());

		JsonValue result;
		result.type = JsonValue::Type::Object;

		JsonValue toolsArr;
		toolsArr.type = JsonValue::Type::Array;

		for (const auto& desc : allDescriptors)
		{
			// 1. 只读降级模式或未绑定会话角色时：仅暴露只读查询与 session.open
			if (readOnlyFallback || m_CurrentRole.empty())
			{
				if (desc.effect != Ai::CommandEffect::Read && desc.name != "session.open")
					continue;
			}
			else
			{
				// 2. 检查角色运行期状态 (D13: DORMANT / DISABLED 休眠角色不分配写与执行工具)
				const auto roleState = core.GetRoleState(m_CurrentRole);
				if (roleState != Agent::RoleRuntimeState::Active)
				{
					if (desc.effect != Ai::CommandEffect::Read && desc.name.rfind("session.", 0) != 0)
						continue;
				}

				// 3. QA / 测试类角色强制过滤代码与资产写入工具 (§3.2)
				const bool isQaRole = (m_CurrentRole == "qa" || m_CurrentRole == "tester" ||
					m_CurrentRole.find("qa") != std::string::npos ||
					m_CurrentRole.find("tester") != std::string::npos);
				if (isQaRole)
				{
					if (desc.effect == Ai::CommandEffect::Write &&
						desc.name.rfind("session.", 0) != 0 &&
						desc.name.rfind("workflow.", 0) != 0 &&
						desc.name.rfind("bug.", 0) != 0 &&
						desc.name.rfind("review.", 0) != 0)
					{
						continue;
					}
				}

				// 4. 委托当前激活的 IWorkflowPolicy 按 (角色 × 阶段) 动态判定命令白名单
				if (policy && desc.effect != Ai::CommandEffect::Read &&
					desc.name.rfind("session.", 0) != 0 &&
					desc.name != "workflow.status")
				{
					if (!policy->CanRoleExecuteCommand(m_CurrentRole, currentStage, desc.name))
						continue;
				}
			}

			JsonValue tool;
			tool.type = JsonValue::Type::Object;
			tool.Object.push_back({ "name", JsonValue::MakeString(desc.name) });
			tool.Object.push_back({ "description", JsonValue::MakeString(desc.description) });

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

		// §3.1 自动注入当前活动会话 Token
		if (!m_CurrentSessionToken.empty() && !args.count("session"))
		{
			args["session"] = m_CurrentSessionToken;
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

		// §3.2 会话切换、关闭、阶段推进/回退或职能开关变更后，更新状态并广播 notifications/tools/list_changed
		if (ok)
		{
			if (cmd == "session.open")
			{
				if (args.count("role"))
					m_CurrentRole = args.at("role");

				auto parsedRes = JsonValue::Parse(outResult, nullptr);
				if (parsedRes && parsedRes->type == JsonValue::Type::Object)
				{
					if (const auto* tokVal = parsedRes->Find("token"))
						m_CurrentSessionToken = tokVal->AsString();
				}

				if (outShouldNotifyListChanged)
					*outShouldNotifyListChanged = true;
			}
			else if (cmd == "session.close")
			{
				m_CurrentSessionToken.clear();
				m_CurrentRole.clear();
				if (outShouldNotifyListChanged)
					*outShouldNotifyListChanged = true;
			}
			else if (cmd == "workflow.advance" || cmd == "workflow.rollback" ||
			         cmd == "role.set_state" || cmd == "role.generate_plan")
			{
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
