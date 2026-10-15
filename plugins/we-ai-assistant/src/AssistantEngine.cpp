#include "AssistantEngine.h"
#include "World/Ai/CommandRegistry.h"
#include "World/WUI/WuiJson.h"

#include <sstream>

namespace World::Assistant
{
	using namespace World::Wui;

	std::string AssistantEngine::Chat(const std::string& message, const std::string& roleId)
	{
		const std::string role = roleId.empty() ? "producer" : roleId;

		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "role", JsonValue::MakeString(role) });

		std::ostringstream reply;
		if (role == "producer")
		{
			reply << "【L1 制作人】已收到您的指导：「" << message << "」。正在组织创意总监与技术部推进当前阶段目标。";
		}
		else if (role == "qa")
		{
			reply << "【QA 质检】自动化门禁校验中，请使用 playtest.run 与 project.validate 自证。";
		}
		else
		{
			reply << "【" << role << "】收到指令，正在执行。";
		}

		root.Object.push_back({ "reply", JsonValue::MakeString(reply.str()) });
		return root.Dump();
	}

	void AssistantEngine::RegisterCommands(World::Ai::CommandRegistry& reg)
	{
		using namespace World::Ai;

		CommandDescriptor chatCmd;
		chatCmd.name = "assistant.chat";
		chatCmd.description = "Conversational chat with in-editor AI Producer and Department Directors.";
		chatCmd.effect = CommandEffect::Read;
		chatCmd.scope = CommandScope::Both;
		chatCmd.owner = "plugin:we-ai-assistant";
		chatCmd.paramsSchema = "{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"},\"role\":{\"type\":\"string\"}},\"required\":[\"message\"]}";
		chatCmd.resultSchema = "{\"type\":\"object\",\"properties\":{\"reply\":{\"type\":\"string\"},\"role\":{\"type\":\"string\"}}}";
		chatCmd.handler = [](const std::map<std::string, std::string>& args, std::string& result, std::string& error)
		{
			const std::string msg = args.count("message") ? args.at("message") : "";
			if (msg.empty())
			{
				error = "Missing message parameter";
				return false;
			}
			const std::string role = args.count("role") ? args.at("role") : "producer";
			result = Chat(msg, role);
			return true;
		};
		reg.RegisterCommand(std::move(chatCmd));
	}
}
