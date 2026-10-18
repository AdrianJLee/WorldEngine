#include "wldpch.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Ai/VfxCommands.h"
#include "World/Ai/SelfValidation.h"
#include "World/Ai/PlaytestRunner.h"
#include "World/Ai/ProjectPackager.h"
#include "World/Ai/WriteGuard.h"
#include "World/Ai/ChangesetTracker.h"
#include "World/Agent/WeAgentCore.h"
#include "World/WUI/WuiJson.h"

#include <algorithm>
#include <sstream>

namespace World::Ai
{
	const char* CommandEffectToString(CommandEffect effect)
	{
		switch (effect)
		{
		case CommandEffect::Read:  return "read";
		case CommandEffect::Write: return "write";
		case CommandEffect::Exec:  return "exec";
		default:                   return "read";
		}
	}

	CommandEffect StringToCommandEffect(const std::string& str)
	{
		if (str == "write") return CommandEffect::Write;
		if (str == "exec")  return CommandEffect::Exec;
		return CommandEffect::Read;
	}

	const char* CommandScopeToString(CommandScope scope)
	{
		switch (scope)
		{
		case CommandScope::EditorOnly:  return "editor";
		case CommandScope::RuntimeOnly: return "runtime";
		case CommandScope::Both:        return "both";
		default:                        return "both";
		}
	}

	CommandScope StringToCommandScope(const std::string& str)
	{
		if (str == "editor")  return CommandScope::EditorOnly;
		if (str == "runtime") return CommandScope::RuntimeOnly;
		return CommandScope::Both;
	}

	CommandRegistry& CommandRegistry::Get()
	{
		static CommandRegistry instance;
		return instance;
	}

	CommandRegistry::CommandRegistry()
	{
		RegisterBuiltinCommands();
	}

	void CommandRegistry::RegisterBuiltinCommands()
	{
		CommandDescriptor capabilitiesCmd;
		capabilitiesCmd.name = "ai.capabilities";
		capabilitiesCmd.description = "Export full machine-readable JSON schema of all registered AI control commands.";
		capabilitiesCmd.effect = CommandEffect::Read;
		capabilitiesCmd.scope = CommandScope::Both;
		capabilitiesCmd.owner = "engine";
		capabilitiesCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"scope\":{\"type\":\"string\",\"enum\":[\"editor\",\"runtime\",\"both\"]},\"effect\":{\"type\":\"string\",\"enum\":[\"read\",\"write\",\"exec\"]}}}";
		capabilitiesCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"capabilities\":{\"type\":\"array\",\"items\":{\"type\":\"object\"}}}}";
		capabilitiesCmd.handler = [this](const std::map<std::string, std::string>& args,
			std::string& result, std::string& /*error*/) -> bool
		{
			const std::string filterScope = args.count("scope") ? args.at("scope") : "";
			const std::string filterEffect = args.count("effect") ? args.at("effect") : "";
			result = ExportCapabilities(filterScope, filterEffect);
			return true;
		};

				RegisterCommand(std::move(capabilitiesCmd));
		SelfValidation::RegisterCommands(*this);
		PlaytestRunner::RegisterCommands(*this);
		ProjectPackager::RegisterCommands(*this);
		ChangesetTracker::RegisterCommands(*this);
		Agent::WeAgentCore::RegisterCommands(*this);
		VfxCommands::RegisterCommands(*this);
	}

	bool CommandRegistry::RegisterCommand(CommandDescriptor desc)
	{
		if (desc.name.empty())
			return false;

		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Commands[desc.name] = std::move(desc);
		return true;
	}

	bool CommandRegistry::UnregisterCommand(const std::string& name)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Commands.find(name);
		if (it != m_Commands.end())
		{
			m_Commands.erase(it);
			return true;
		}
		return false;
	}

	bool CommandRegistry::HasCommand(const std::string& name) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return m_Commands.find(name) != m_Commands.end();
	}

	const CommandDescriptor* CommandRegistry::GetDescriptor(const std::string& name) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		auto it = m_Commands.find(name);
		if (it != m_Commands.end())
			return &it->second;
		return nullptr;
	}

	std::vector<CommandDescriptor> CommandRegistry::GetAllDescriptors() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		std::vector<CommandDescriptor> list;
		list.reserve(m_Commands.size());
		for (const auto& kv : m_Commands)
		{
			list.push_back(kv.second);
		}
		return list;
	}

	bool CommandRegistry::Dispatch(const std::string& name,
		const std::map<std::string, std::string>& args,
		std::string& result,
		std::string& error)
	{
		CommandDescriptor desc;
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			auto it = m_Commands.find(name);
			if (it == m_Commands.end())
			{
				error = "unknown command '" + name + "'";
				return false;
			}
			desc = it->second;
		}

		if (!desc.handler)
		{
			error = "handler not implemented for command '" + name + "'";
			return false;
		}

		// 会话与策略守卫
		std::string token = args.count("session") ? args.at("session") : "";
		if (token.empty())
		{
			token = Agent::WeAgentCore::Get().GetActiveSessionToken();
		}

		if (!token.empty())
		{
			const auto* sess = Agent::WeAgentCore::Get().GetSession(token);
			if (sess)
			{
				// 1. 角色运行期状态检查 (D13)
				const auto roleState = Agent::WeAgentCore::Get().GetRoleState(sess->roleId);
				if (roleState != Agent::RoleRuntimeState::Active && desc.effect != CommandEffect::Read && name.rfind("session.", 0) != 0)
				{
					error = "Forbidden (D13): role '" + sess->roleId + "' is currently " +
						Agent::RoleRuntimeStateToString(roleState) + "; cannot execute non-read command '" + name + "'";
					return false;
				}

				// 2. 策略白名单检查
				auto policy = Agent::WeAgentCore::Get().GetPolicy();
				if (policy && desc.effect != CommandEffect::Read &&
					name.rfind("session.", 0) != 0 &&
					name != "workflow.status" &&
					name != "ai.capabilities")
				{
					if (!policy->CanRoleExecuteCommand(sess->roleId, sess->activeStage, name))
					{
						error = "Forbidden by policy: role '" + sess->roleId + "' cannot execute command '" + name +
							"' in stage " + Agent::WorkflowStageToString(sess->activeStage);
						return false;
					}
				}

				// 3. 写入守卫与通道写入记录
				if (desc.effect == CommandEffect::Write)
				{
					std::string pathArg;
					if (args.count("path")) pathArg = args.at("path");
					else if (args.count("target")) pathArg = args.at("target");
					else if (args.count("file")) pathArg = args.at("file");

					if (!pathArg.empty())
					{
						std::string guardReason;
						WritePermission perm;
						perm.roleId = sess->roleId;
						if (!WriteGuard::Get().CanWrite(pathArg, sess->projectRoot, &perm, guardReason))
						{
							error = guardReason;
							return false;
						}
						WriteGuard::Get().RecordWrite(pathArg, sess->projectRoot, sess->roleId, sess->token);
					}
				}
			}
		}

		return desc.handler(args, result, error);
	}

	std::string CommandRegistry::ExportCapabilities(const std::string& filterScope,
		const std::string& filterEffect) const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);

		using namespace World::Wui;
		JsonValue root;
		root.type = JsonValue::Type::Object;

		JsonValue capsArray;
		capsArray.type = JsonValue::Type::Array;

		for (const auto& kv : m_Commands)
		{
			const auto& desc = kv.second;

			if (!filterScope.empty())
			{
				const std::string scopeStr = CommandScopeToString(desc.scope);
				if (filterScope != "both" && scopeStr != "both" && scopeStr != filterScope)
					continue;
			}

			if (!filterEffect.empty())
			{
				const std::string effectStr = CommandEffectToString(desc.effect);
				if (effectStr != filterEffect)
					continue;
			}

			JsonValue item;
			item.type = JsonValue::Type::Object;
			item.Object.push_back({ "name", JsonValue::MakeString(desc.name) });
			item.Object.push_back({ "description", JsonValue::MakeString(desc.description) });
			item.Object.push_back({ "effect", JsonValue::MakeString(CommandEffectToString(desc.effect)) });
			item.Object.push_back({ "scope", JsonValue::MakeString(CommandScopeToString(desc.scope)) });
			item.Object.push_back({ "owner", JsonValue::MakeString(desc.owner) });

			// 解析 paramsSchema，如果非法则作为空对象包装
			std::string errParams;
			auto parsedParams = JsonValue::Parse(desc.paramsSchema, &errParams);
			item.Object.push_back({ "params_schema", parsedParams.has_value() ? std::move(*parsedParams) : JsonValue::Parse("{}", nullptr).value() });

			// 解析 resultSchema
			std::string errResult;
			auto parsedResult = JsonValue::Parse(desc.resultSchema, &errResult);
			item.Object.push_back({ "result_schema", parsedResult.has_value() ? std::move(*parsedResult) : JsonValue::Parse("{}", nullptr).value() });

			capsArray.Array.push_back(std::move(item));
		}

		root.Object.push_back({ "capabilities", std::move(capsArray) });
		return root.Dump();
	}

	void CommandRegistry::ClearUserCommands()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		for (auto it = m_Commands.begin(); it != m_Commands.end(); )
		{
			if (it->second.owner != "engine")
				it = m_Commands.erase(it);
			else
				++it;
		}
	}

	void CommandRegistry::ResetAll()
	{
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Commands.clear();
		}
		RegisterBuiltinCommands();
	}
}

