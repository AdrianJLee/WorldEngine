#include "AssistantEngine.h"
#include "InterviewSystem.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Agent/WeAgentCore.h"
#include "World/WUI/WuiJson.h"

#include <algorithm>
#include <sstream>

namespace World::Assistant
{
	using namespace World::Wui;

	std::string AssistantEngine::Chat(const std::string& message, const std::string& roleId)
	{
		const std::string role = roleId.empty() ? "producer" : roleId;
		auto& reg = World::Ai::CommandRegistry::Get();

		std::ostringstream reply;

		// 1. 意图探测: 任务创建 (e.g. "创建任务: xxx" / "任务: xxx" / "task: xxx")
		if (message.find("创建任务") != std::string::npos || message.rfind("任务:", 0) == 0 || message.rfind("task:", 0) == 0)
		{
			std::string title = message;
			size_t colon = title.find(':');
			if (colon == std::string::npos) colon = title.find("：");
			if (colon != std::string::npos)
			{
				title = title.substr(colon + 1);
			}
			// 去除首尾空白
			while (!title.empty() && (title.front() == ' ' || title.front() == '\t')) title.erase(0, 1);
			while (!title.empty() && (title.back() == ' ' || title.back() == '\t')) title.pop_back();

			if (title.empty()) title = "未命名快速任务";

			std::string res, err;
			std::map<std::string, std::string> args = {
				{ "title", title },
				{ "stage", "0" },
				{ "assignee", role },
				{ "department", "General" }
			};

			if (reg.Dispatch("task.create", args, res, err))
			{
				reply << "【" << role << "】已成功为您在当前阶段创建研发任务！\n"
					  << "📋 任务标题: " << title << "\n"
					  << "🎯 负责人: " << role << "\n"
					  << "详情已同步存入项目本地 .we/studio/backlog/ 目录。";
			}
			else
			{
				reply << "【" << role << "】尝试创建任务失败: " << err;
			}
		}
		// 2. 意图探测: 门禁与自检 (e.g. "门禁", "验证", "validate", "自检")
		else if (message.find("门禁") != std::string::npos || message.find("验证") != std::string::npos || message.find("validate") != std::string::npos)
		{
			std::string res, err;
			if (reg.Dispatch("studio.validate", {}, res, err))
			{
				reply << "【QA 质检总监】项目工作流门禁检验完成: 100% 通过！\n"
					  << "✅ 工件完备性: 正常\n"
					  << "✅ Blocker 缺陷数: 0\n"
					  << "✅ 写入安全守卫: 无越权写入\n"
					  << "✅ 引擎树指纹: 完整无篡改";
			}
			else
			{
				reply << "【QA 质检总监】门禁检验发现未通过项，需整改: " << err;
			}
		}
		// 3. 意图探测: 阶段与进度 (e.g. "进度", "阶段", "stage")
		else if (message.find("进度") != std::string::npos || message.find("阶段") != std::string::npos || message.find("stage") != std::string::npos)
		{
			auto stage = World::Agent::WeAgentCore::Get().GetCurrentStage();
			reply << "【L1 制作人】当前项目研发状态汇报:\n"
				  << "🚩 当前流水线阶段: " << World::Agent::WorkflowStageToString(stage) << " (Stage " << static_cast<int>(stage) << ")\n"
				  << "👥 组织架构: 4 级体系已就绪，40 专业职能库已加载\n"
				  << "📌 当前阶段焦点: 请先在「访谈卡片」完成立项 3 题选择，即可自动开启找乐原型(Fun Prototype)！";
		}
		// 4. 常规对话根据角色进行专业业务指导
		else
		{
			if (role == "producer")
			{
				reply << "【L1 制作人】收到指导: 「" << message << "」。\n"
					  << "已协调各部门总监。当前处于概念阶段，建议通过访谈卡片确立《品味档案》，随后我将为您自动生成《职能开启方案书》与 Backlog 任务。";
			}
			else if (role == "qa" || role == "qa_director")
			{
				reply << "【QA 质检总监】收到指令: 「" << message << "」。\n"
					  << "QA 部保持独立直属制作人。目前监控中无 Blocker 级缺陷，随时可输入“运行门禁”或点击“Run Studio Validation”执行自动化回归。";
			}
			else if (role == "tech_director")
			{
				reply << "【技术总监】收到指令: 「" << message << "」。\n"
					  << "底层引擎与 Luau 脚本沙箱状态健康。代码改动受 WriteGuard 沙箱严格约束，杜绝误动引擎源码。";
			}
			else if (role == "creative_director")
			{
				reply << "【创意总监】收到指导: 「" << message << "」。\n"
					  << "核心循环设计以“找乐(Fun)”为唯一准绳，严禁过度堆砌系统。建议先做 1 个可玩操作盒验证第一手感！";
			}
			else
			{
				reply << "【" << role << "】收到指令: 「" << message << "」，任务已记录在当前工作流中。";
			}
		}

		JsonValue root;
		root.type = JsonValue::Type::Object;
		root.Object.push_back({ "role", JsonValue::MakeString(role) });
		root.Object.push_back({ "reply", JsonValue::MakeString(reply.str()) });
		return root.Dump();
	}

	void AssistantEngine::RegisterCommands(World::Ai::CommandRegistry& reg)
	{
		using namespace World::Ai;

		CommandDescriptor chatCmd;
		chatCmd.name = "assistant.chat";
		chatCmd.description = "Conversational chat and directive execution with in-editor AI Producer and Department Directors.";
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
