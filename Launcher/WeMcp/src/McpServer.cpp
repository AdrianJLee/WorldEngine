#include "McpServer.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Agent/WeAgentCore.h"
#include "World/Utils/Paths.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>

namespace World::Mcp
{
	using namespace World::Wui;

	McpServer::McpServer()
		: m_Bridge(std::make_shared<EngineBridge>())
	{
		ResetToolGroups();
	}

	void McpServer::SetCurrentRole(const std::string& role)
	{
		m_CurrentRole = role;
	}

	void McpServer::ActivateToolGroup(const std::string& group)
	{
		if (!group.empty())
		{
			m_ActiveToolGroups.insert(group);
		}
	}

	void McpServer::DeactivateToolGroup(const std::string& group)
	{
		if (group != "core")
		{
			m_ActiveToolGroups.erase(group);
		}
	}

	void McpServer::ResetToolGroups()
	{
		m_ActiveToolGroups = { "core", "scene", "asset", "playtest" };
	}

	std::string McpServer::ClassifyToolGroup(const std::string& cmdName)
	{
		std::string norm = cmdName;
		for (char& c : norm)
		{
			if (c == '_') c = '.';
		}

		if (norm == "manage.tools" || norm == "batch.execute") return "core";
		if (norm.rfind("session.", 0) == 0) return "core";
		if (norm.rfind("workflow.", 0) == 0) return "core";
		if (norm.rfind("gate.", 0) == 0) return "core";
		if (norm.rfind("role.", 0) == 0) return "core";
		if (norm.rfind("changeset.", 0) == 0) return "core";
		if (norm.rfind("project.", 0) == 0) return "core";
		if (norm.rfind("engine.", 0) == 0) return "core";
		if (norm == "ai.capabilities") return "core";

		if (norm.rfind("scene.", 0) == 0) return "scene";
		if (norm.rfind("schema.", 0) == 0) return "scene";
		if (norm == "state.dump" || norm == "stats.scene") return "scene";

		if (norm.rfind("asset.", 0) == 0) return "asset";
		if (norm.rfind("material.", 0) == 0) return "asset";
		if (norm.rfind("datatable.", 0) == 0) return "asset";

		if (norm.rfind("playtest.", 0) == 0) return "playtest";
		if (norm.rfind("play.", 0) == 0) return "playtest";
		if (norm.rfind("capture.", 0) == 0) return "playtest";
		if (norm.rfind("log.", 0) == 0 || norm.rfind("ops.", 0) == 0) return "playtest";

		if (norm.rfind("vfx.", 0) == 0) return "vfx";
		if (norm.rfind("audio.", 0) == 0) return "audio";
		if (norm.rfind("navmesh.", 0) == 0) return "navmesh";

		if (norm.rfind("ui.", 0) == 0) return "ui";
		if (norm.rfind("viewport.", 0) == 0) return "ui";
		if (norm.rfind("terrain.", 0) == 0) return "ui";

		if (norm.rfind("prof.", 0) == 0) return "profiling";

		return "core";
	}

	std::string McpServer::HandleInitialize(const JsonValue& id, const JsonValue& /*params*/)
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

		JsonValue resourcesCap;
		resourcesCap.type = JsonValue::Type::Object;
		resourcesCap.Object.push_back({ "subscribe", JsonValue::MakeBool(false) });
		resourcesCap.Object.push_back({ "listChanged", JsonValue::MakeBool(true) });
		caps.Object.push_back({ "resources", std::move(resourcesCap) });

		result.Object.push_back({ "capabilities", std::move(caps) });

		result.Object.push_back({ "instructions", JsonValue::MakeString(R"raw(You are an AI Game Producer and Department Director operating inside WorldEngine.

AUTHORING FIRST. A "game" is an authored scene + scripts + real assets that actually runs.

Mandatory loop:
1. Open a session (session.open) and validate the project (project.validate).
2. Use 'batch_execute' for high-throughput batch operations (creating entities and assigning multiple components in one roundtrip).
3. Use 'manage_tools' to activate domain-specific tool groups (e.g. vfx, audio, navmesh, ui, profiling) as needed.
4. Read MCP resources (worldengine://project/info, worldengine://schemas, worldengine://api/stubs) to inspect capabilities without consuming tool rounds.
5. Author the level with real commands (scene.open, scene.create_entity, scene.add_component, scene.set). Persist with scene.save.
6. Write Luau scripts through changeset.begin -> changeset.stage_write -> changeset.apply.
7. Verify with playtest.run on the saved level.
)raw") });

		McpResponse resp;
		resp.id = id;
		resp.result = std::move(result);
		return resp.ToJson();
	}

	std::string McpServer::HandleToolsList(const JsonValue& id)
	{
		if (m_Bridge)
		{
			m_Bridge->EnsureInitialized();
		}

		auto allDescriptors = Ai::CommandRegistry::Get().GetAllDescriptors();

		// 内置核心工具: manage_tools 与 batch_execute
		{
			Ai::CommandDescriptor manageToolsDesc;
			manageToolsDesc.name = "manage_tools";
			manageToolsDesc.description = "Show, enable, or hide tool groups for the current session. Actions: 'list_groups' (returns catalog & status), 'activate' (enables a group: vfx, audio, navmesh, ui, profiling), 'deactivate' (hides a group), 'reset' (restore defaults).";
			manageToolsDesc.effect = Ai::CommandEffect::Read;
			manageToolsDesc.paramsSchema = R"raw({
  "type": "object",
  "properties": {
    "action": {
      "type": "string",
      "description": "Action to perform: 'list_groups', 'activate', 'deactivate', 'reset'",
      "enum": ["list_groups", "activate", "deactivate", "reset"]
    },
    "group": {
      "type": "string",
      "description": "Target tool group name, e.g. 'vfx', 'audio', 'navmesh', 'ui', 'profiling', 'scene', 'asset', 'playtest'"
    }
  },
  "required": ["action"]
})raw";
			allDescriptors.push_back(std::move(manageToolsDesc));

			Ai::CommandDescriptor batchExecDesc;
			batchExecDesc.name = "batch_execute";
			batchExecDesc.description = "Execute multiple MCP tool commands sequentially in a single batch request (10-50x faster, avoiding multi-turn latency).";
			batchExecDesc.effect = Ai::CommandEffect::Write;
			batchExecDesc.paramsSchema = R"raw({
  "type": "object",
  "properties": {
    "commands": {
      "type": "array",
      "description": "List of command objects to execute in order. Each object contains 'tool' (or 'cmd') and 'params' (or 'args').",
      "items": {
        "type": "object",
        "properties": {
          "tool": { "type": "string", "description": "Command name, e.g. scene.create_entity" },
          "params": { "type": "object", "description": "Key-value argument dictionary" }
        },
        "required": ["tool"]
      }
    },
    "fail_fast": {
      "type": "boolean",
      "description": "If true, stops execution upon first command failure. Defaults to false."
    }
  },
  "required": ["commands"]
})raw";
			allDescriptors.push_back(std::move(batchExecDesc));
		}

		// 远端宿主(Editor)的完整创作命令表
		if (m_Bridge && m_Bridge->IsConnectedToRemote())
		{
			if (!m_RemoteCommandsLoaded)
			{
				for (int attempt = 0; attempt < 5 && m_RemoteCommands.empty(); ++attempt)
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
									m_RemoteCommands.push_back(std::move(d));
								}
							}
						}
					}
					if (m_RemoteCommands.empty() && m_Bridge->IsConnectedToRemote())
						std::this_thread::sleep_for(std::chrono::milliseconds(80));
					else if (!m_Bridge->IsConnectedToRemote())
						break;
				}
				m_RemoteCommandsLoaded = true;
			}
			for (const auto& remote : m_RemoteCommands)
				allDescriptors.push_back(remote);
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
			// 0. 动态工具组过滤 (Tool Groups)
			const std::string group = ClassifyToolGroup(desc.name);
			if (m_ActiveToolGroups.find(group) == m_ActiveToolGroups.end())
			{
				continue;
			}

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
					desc.name != "workflow.status" &&
					desc.name != "manage_tools" &&
					desc.name != "batch_execute")
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
				// MCP 规范要求 inputSchema 根节点必须带有 "type": "object"
				if (!parsedSchema->Find("type"))
				{
					parsedSchema->Object.insert(parsedSchema->Object.begin(), { "type", JsonValue::MakeString("object") });
				}
				if (!parsedSchema->Find("properties"))
				{
					JsonValue emptyProps;
					emptyProps.type = JsonValue::Type::Object;
					parsedSchema->Object.push_back({ "properties", std::move(emptyProps) });
				}
				tool.Object.push_back({ "inputSchema", std::move(*parsedSchema) });
			}
			else
			{
				JsonValue defaultSchema;
				defaultSchema.type = JsonValue::Type::Object;
				defaultSchema.Object.push_back({ "type", JsonValue::MakeString("object") });
				JsonValue emptyProps;
				emptyProps.type = JsonValue::Type::Object;
				defaultSchema.Object.push_back({ "properties", std::move(emptyProps) });
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

	bool McpServer::DispatchSingleTool(const std::string& inputCmd,
		const std::map<std::string, std::string>& inArgs,
		std::string& outResult,
		std::string& outError,
		bool* outShouldNotifyListChanged)
	{
		std::string cmd = inputCmd;
		if (!Ai::CommandRegistry::Get().HasCommand(cmd))
		{
			const size_t firstUnderscore = cmd.find('_');
			if (firstUnderscore != std::string::npos)
			{
				std::string dotCandidate = cmd;
				dotCandidate[firstUnderscore] = '.';
				if (Ai::CommandRegistry::Get().HasCommand(dotCandidate))
				{
					cmd = dotCandidate;
				}
			}
		}

		std::map<std::string, std::string> args = inArgs;
		if (!m_CurrentSessionToken.empty() && !args.count("session"))
		{
			args["session"] = m_CurrentSessionToken;
		}

		bool ok = false;
		// 性能优化: 引擎内建静态只读指令与校验 (project.validate, asset.validate, schema.*, project.info, asset.list 等)
		// 优先在 WeMcp 进程内微秒级执行，不发起跨进程 socket 亦不等待拉起 Editor 进程 (从 5000ms 骤降至 15ms)
		const auto* localDesc = Ai::CommandRegistry::Get().GetDescriptor(cmd);
		const bool isFastLocal = localDesc && (localDesc->effect == Ai::CommandEffect::Read || localDesc->owner == "engine")
			&& cmd != "capture.scene" && cmd != "capture.screen" && cmd != "capture.float" && cmd != "capture.texture"
			&& cmd != "state.dump" && cmd != "stats.scene" && cmd != "material.get";

		if (isFastLocal && (!m_Bridge || !m_Bridge->IsConnectedToRemote()))
		{
			ok = Ai::CommandRegistry::Get().Dispatch(cmd, args, outResult, outError);
		}
		else if (m_Bridge)
		{
			ok = m_Bridge->Dispatch(cmd, args, outResult, outError);
		}
		else
		{
			ok = Ai::CommandRegistry::Get().Dispatch(cmd, args, outResult, outError);
		}

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

		return ok;
	}

	std::string McpServer::HandleManageTools(const JsonValue& id, const JsonValue& params, bool* outShouldNotifyListChanged)
	{
		const JsonValue* argsObj = params.Find("arguments");
		if (!argsObj || argsObj->type != JsonValue::Type::Object)
			argsObj = &params;

		std::string action;
		if (const auto* aVal = argsObj->Find("action"))
			action = aVal->AsString();

		std::string group;
		if (const auto* gVal = argsObj->Find("group"))
			group = gVal->AsString();

		std::vector<std::string> allKnownGroups = {
			"core", "scene", "asset", "playtest", "vfx", "audio", "navmesh", "ui", "profiling"
		};

		std::string message;
		bool ok = true;

		if (action == "list_groups")
		{
			JsonValue groupsObj;
			groupsObj.type = JsonValue::Type::Object;

			JsonValue grpArr;
			grpArr.type = JsonValue::Type::Array;
			for (const auto& g : allKnownGroups)
			{
				JsonValue item;
				item.type = JsonValue::Type::Object;
				item.Object.push_back({ "group", JsonValue::MakeString(g) });
				item.Object.push_back({ "active", JsonValue::MakeBool(m_ActiveToolGroups.count(g) > 0) });
				grpArr.Array.push_back(std::move(item));
			}
			groupsObj.Object.push_back({ "groups", std::move(grpArr) });
			message = groupsObj.Dump();
		}
		else if (action == "activate")
		{
			if (group.empty())
			{
				ok = false;
				message = "Missing 'group' parameter for activate";
			}
			else
			{
				ActivateToolGroup(group);
				message = "Activated tool group: " + group;
				if (outShouldNotifyListChanged)
					*outShouldNotifyListChanged = true;
			}
		}
		else if (action == "deactivate")
		{
			if (group == "core")
			{
				ok = false;
				message = "Cannot deactivate 'core' tool group";
			}
			else if (group.empty())
			{
				ok = false;
				message = "Missing 'group' parameter for deactivate";
			}
			else
			{
				DeactivateToolGroup(group);
				message = "Deactivated tool group: " + group;
				if (outShouldNotifyListChanged)
					*outShouldNotifyListChanged = true;
			}
		}
		else if (action == "reset")
		{
			ResetToolGroups();
			message = "Reset tool groups to defaults (core, scene, asset, playtest)";
			if (outShouldNotifyListChanged)
				*outShouldNotifyListChanged = true;
		}
		else
		{
			ok = false;
			message = "Unknown manage_tools action: " + action;
		}

		JsonValue contentArr;
		contentArr.type = JsonValue::Type::Array;
		JsonValue textItem;
		textItem.type = JsonValue::Type::Object;
		textItem.Object.push_back({ "type", JsonValue::MakeString("text") });
		textItem.Object.push_back({ "text", JsonValue::MakeString(message) });
		contentArr.Array.push_back(std::move(textItem));

		JsonValue finalResult;
		finalResult.type = JsonValue::Type::Object;
		finalResult.Object.push_back({ "content", std::move(contentArr) });
		finalResult.Object.push_back({ "isError", JsonValue::MakeBool(!ok) });

		McpResponse resp;
		resp.id = id;
		resp.result = std::move(finalResult);
		return resp.ToJson();
	}

	std::string McpServer::HandleBatchExecute(const JsonValue& id, const JsonValue& params, bool* outShouldNotifyListChanged)
	{
		const JsonValue* argsObj = params.Find("arguments");
		if (!argsObj || argsObj->type != JsonValue::Type::Object)
			argsObj = &params;

		const auto* cmdsVal = argsObj->Find("commands");
		if (!cmdsVal || cmdsVal->type != JsonValue::Type::Array)
		{
			McpResponse resp;
			resp.id = id;
			resp.isError = true;
			resp.errorCode = -32602;
			resp.errorMessage = "Missing or invalid 'commands' array for batch_execute";
			return resp.ToJson();
		}

		bool failFast = false;
		if (const auto* ffVal = argsObj->Find("fail_fast"))
			failFast = ffVal->AsBool();

		JsonValue batchResult;
		batchResult.type = JsonValue::Type::Object;

		JsonValue resultsArr;
		resultsArr.type = JsonValue::Type::Array;

		int total = static_cast<int>(cmdsVal->Array.size());
		int executed = 0;
		int succeeded = 0;
		int failed = 0;

		for (int i = 0; i < total; ++i)
		{
			const auto& item = cmdsVal->Array[i];
			if (item.type != JsonValue::Type::Object)
				continue;

			std::string subTool;
			if (const auto* tVal = item.Find("tool")) subTool = tVal->AsString();
			else if (const auto* cVal = item.Find("cmd")) subTool = cVal->AsString();
			else if (const auto* nVal = item.Find("name")) subTool = nVal->AsString();

			if (subTool.empty())
			{
				JsonValue resItem;
				resItem.type = JsonValue::Type::Object;
				resItem.Object.push_back({ "index", JsonValue::MakeNumber(i) });
				resItem.Object.push_back({ "ok", JsonValue::MakeBool(false) });
				resItem.Object.push_back({ "error", JsonValue::MakeString("Missing command name in batch item") });
				resultsArr.Array.push_back(std::move(resItem));
				++failed;
				++executed;
				if (failFast) break;
				continue;
			}

			if (subTool == "batch_execute")
			{
				JsonValue resItem;
				resItem.type = JsonValue::Type::Object;
				resItem.Object.push_back({ "index", JsonValue::MakeNumber(i) });
				resItem.Object.push_back({ "tool", JsonValue::MakeString(subTool) });
				resItem.Object.push_back({ "ok", JsonValue::MakeBool(false) });
				resItem.Object.push_back({ "error", JsonValue::MakeString("Nested batch_execute is not permitted") });
				resultsArr.Array.push_back(std::move(resItem));
				++failed;
				++executed;
				if (failFast) break;
				continue;
			}

			std::map<std::string, std::string> subArgs;
			const JsonValue* pObj = item.Find("params");
			if (!pObj) pObj = item.Find("arguments");
			if (!pObj) pObj = item.Find("args");

			if (pObj && pObj->type == JsonValue::Type::Object)
			{
				for (const auto& kv : pObj->Object)
				{
					if (kv.second.type == JsonValue::Type::String)
						subArgs[kv.first] = kv.second.AsString();
					else if (kv.second.type == JsonValue::Type::Number)
					{
						const double n = kv.second.AsNumber();
						if (std::floor(n) == n && n >= -9007199254740992.0 && n <= 9007199254740992.0)
							subArgs[kv.first] = std::to_string(static_cast<long long>(n));
						else
							subArgs[kv.first] = std::to_string(n);
					}
					else if (kv.second.type == JsonValue::Type::Bool)
						subArgs[kv.first] = kv.second.AsBool() ? "true" : "false";
					else
						subArgs[kv.first] = kv.second.Dump();
				}
			}

			std::string subRes, subErr;
			bool subOk = DispatchSingleTool(subTool, subArgs, subRes, subErr, outShouldNotifyListChanged);
			++executed;

			JsonValue resItem;
			resItem.type = JsonValue::Type::Object;
			resItem.Object.push_back({ "index", JsonValue::MakeNumber(i) });
			resItem.Object.push_back({ "tool", JsonValue::MakeString(subTool) });
			resItem.Object.push_back({ "ok", JsonValue::MakeBool(subOk) });
			if (subOk)
			{
				++succeeded;
				resItem.Object.push_back({ "result", JsonValue::MakeString(subRes) });
			}
			else
			{
				++failed;
				resItem.Object.push_back({ "error", JsonValue::MakeString(subErr) });
			}
			resultsArr.Array.push_back(std::move(resItem));

			if (!subOk && failFast)
				break;
		}

		batchResult.Object.push_back({ "total", JsonValue::MakeNumber(total) });
		batchResult.Object.push_back({ "executed", JsonValue::MakeNumber(executed) });
		batchResult.Object.push_back({ "succeeded", JsonValue::MakeNumber(succeeded) });
		batchResult.Object.push_back({ "failed", JsonValue::MakeNumber(failed) });
		batchResult.Object.push_back({ "results", std::move(resultsArr) });

		JsonValue contentArr;
		contentArr.type = JsonValue::Type::Array;
		JsonValue textItem;
		textItem.type = JsonValue::Type::Object;
		textItem.Object.push_back({ "type", JsonValue::MakeString("text") });
		textItem.Object.push_back({ "text", JsonValue::MakeString(batchResult.Dump()) });
		contentArr.Array.push_back(std::move(textItem));

		JsonValue finalResult;
		finalResult.type = JsonValue::Type::Object;
		finalResult.Object.push_back({ "content", std::move(contentArr) });
		finalResult.Object.push_back({ "isError", JsonValue::MakeBool(failed > 0 && succeeded == 0) });

		McpResponse resp;
		resp.id = id;
		resp.result = std::move(finalResult);
		return resp.ToJson();
	}

	std::string McpServer::HandleResourcesList(const JsonValue& id)
	{
		JsonValue result;
		result.type = JsonValue::Type::Object;

		JsonValue resArr;
		resArr.type = JsonValue::Type::Array;

		auto addRes = [&](const std::string& uri, const std::string& name, const std::string& desc, const std::string& mime)
		{
			JsonValue item;
			item.type = JsonValue::Type::Object;
			item.Object.push_back({ "uri", JsonValue::MakeString(uri) });
			item.Object.push_back({ "name", JsonValue::MakeString(name) });
			item.Object.push_back({ "description", JsonValue::MakeString(desc) });
			item.Object.push_back({ "mimeType", JsonValue::MakeString(mime) });
			resArr.Array.push_back(std::move(item));
		};

		addRes("worldengine://project/info", "Project Info", "Project configuration, project directory, engine version, and host mode.", "application/json");
		addRes("worldengine://schemas", "ECS Component Schemas", "Complete catalog of registered ECS component schemas, types, and annotations.", "application/json");
		addRes("worldengine://api/stubs", "Luau Script API Stubs", "Complete WorldEngineAPI.luau script stubs for AI code synthesis and Luau autocompletion.", "text/x-lua");
		addRes("worldengine://tool-groups", "Tool Groups Catalog", "Current active and available tool groups and their activation status.", "application/json");

		result.Object.push_back({ "resources", std::move(resArr) });

		McpResponse resp;
		resp.id = id;
		resp.result = std::move(result);
		return resp.ToJson();
	}

	std::string McpServer::HandleResourcesRead(const JsonValue& id, const JsonValue& params)
	{
		const auto* uriVal = params.Find("uri");
		if (!uriVal || uriVal->type != JsonValue::Type::String)
		{
			McpResponse resp;
			resp.id = id;
			resp.isError = true;
			resp.errorCode = -32602;
			resp.errorMessage = "Missing uri parameter in resources/read";
			return resp.ToJson();
		}

		std::string uri = uriVal->AsString();
		std::string textContent;
		std::string mimeType = "application/json";

		if (uri == "worldengine://project/info")
		{
			JsonValue info;
			info.type = JsonValue::Type::Object;
			info.Object.push_back({ "engineVersion", JsonValue::MakeString("1.0.0") });
			info.Object.push_back({ "projectRoot", JsonValue::MakeString(World::Paths::ProjectDir().string()) });
			info.Object.push_back({ "connectedToHost", JsonValue::MakeBool(m_Bridge ? m_Bridge->IsConnectedToRemote() : false) });
			info.Object.push_back({ "remotePort", JsonValue::MakeNumber(m_Bridge ? m_Bridge->RemotePort() : 0) });
			info.Object.push_back({ "currentRole", JsonValue::MakeString(m_CurrentRole) });
			info.Object.push_back({ "sessionToken", JsonValue::MakeString(m_CurrentSessionToken) });
			textContent = info.Dump();
		}
		else if (uri == "worldengine://schemas")
		{
			std::string outRes, outErr;
			if (m_Bridge && m_Bridge->Dispatch("schema.list", {}, outRes, outErr))
				textContent = outRes;
			else if (Ai::CommandRegistry::Get().Dispatch("schema.list", {}, outRes, outErr))
				textContent = outRes;
			else
				textContent = "{\"schemas\":[]}";
		}
		else if (uri == "worldengine://api/stubs")
		{
			mimeType = "text/x-lua";
			std::string outRes, outErr;
			if (m_Bridge && m_Bridge->Dispatch("api.luau_stubs", {}, outRes, outErr))
				textContent = outRes;
			else if (Ai::CommandRegistry::Get().Dispatch("api.luau_stubs", {}, outRes, outErr))
				textContent = outRes;
			else
				textContent = "-- WorldEngineAPI.luau stubs";
		}
		else if (uri == "worldengine://tool-groups")
		{
			std::vector<std::string> allKnownGroups = {
				"core", "scene", "asset", "playtest", "vfx", "audio", "navmesh", "ui", "profiling"
			};
			JsonValue grpArr;
			grpArr.type = JsonValue::Type::Array;
			for (const auto& g : allKnownGroups)
			{
				JsonValue item;
				item.type = JsonValue::Type::Object;
				item.Object.push_back({ "group", JsonValue::MakeString(g) });
				item.Object.push_back({ "active", JsonValue::MakeBool(m_ActiveToolGroups.count(g) > 0) });
				grpArr.Array.push_back(std::move(item));
			}
			JsonValue root;
			root.type = JsonValue::Type::Object;
			root.Object.push_back({ "groups", std::move(grpArr) });
			textContent = root.Dump();
		}
		else
		{
			McpResponse resp;
			resp.id = id;
			resp.isError = true;
			resp.errorCode = -32602;
			resp.errorMessage = "Resource not found: " + uri;
			return resp.ToJson();
		}

		JsonValue contentsArr;
		contentsArr.type = JsonValue::Type::Array;

		JsonValue resItem;
		resItem.type = JsonValue::Type::Object;
		resItem.Object.push_back({ "uri", JsonValue::MakeString(uri) });
		resItem.Object.push_back({ "mimeType", JsonValue::MakeString(mimeType) });
		resItem.Object.push_back({ "text", JsonValue::MakeString(textContent) });
		contentsArr.Array.push_back(std::move(resItem));

		JsonValue result;
		result.type = JsonValue::Type::Object;
		result.Object.push_back({ "contents", std::move(contentsArr) });

		McpResponse resp;
		resp.id = id;
		resp.result = std::move(result);
		return resp.ToJson();
	}

	std::string McpServer::HandleToolsCall(const JsonValue& id, const JsonValue& params, bool* outShouldNotifyListChanged)
	{
		// 性能优化: 不在此处盲目预热阻塞拉起 Editor 宿主，由 DispatchSingleTool 发现非本地命令时按需懒加载
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

		std::string cmd = nameVal->AsString();

		if (cmd == "batch_execute")
		{
			return HandleBatchExecute(id, params, outShouldNotifyListChanged);
		}

		if (cmd == "manage_tools")
		{
			return HandleManageTools(id, params, outShouldNotifyListChanged);
		}

		std::map<std::string, std::string> args;
		const auto* argsObj = params.Find("arguments");
		if (argsObj && argsObj->type == JsonValue::Type::Object)
		{
			for (const auto& kv : argsObj->Object)
			{
				if (kv.second.type == JsonValue::Type::String)
					args[kv.first] = kv.second.AsString();
				else if (kv.second.type == JsonValue::Type::Number)
				{
					const double n = kv.second.AsNumber();
					if (std::floor(n) == n && n >= -9007199254740992.0 && n <= 9007199254740992.0)
						args[kv.first] = std::to_string(static_cast<long long>(n));
					else
						args[kv.first] = std::to_string(n);
				}
				else if (kv.second.type == JsonValue::Type::Bool)
					args[kv.first] = kv.second.AsBool() ? "true" : "false";
				else
					args[kv.first] = kv.second.Dump();
			}
		}

		std::string outResult;
		std::string outError;
		bool ok = DispatchSingleTool(cmd, args, outResult, outError, outShouldNotifyListChanged);

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
			resp.id = JsonValue::Null();
			resp.isError = true;
			resp.errorCode = -32700;
			resp.errorMessage = "Parse error: " + err;
			return resp.ToJson();
		}

		const auto* idVal = reqOpt->Find("id");
		const JsonValue id = idVal ? *idVal : JsonValue::Null();

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
		if (method == "resources/list")
		{
			return HandleResourcesList(id);
		}
		if (method == "resources/read")
		{
			return HandleResourcesRead(id, params);
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
