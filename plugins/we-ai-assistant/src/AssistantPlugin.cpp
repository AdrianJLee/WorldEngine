#include "World/Plugins/WePluginApi.h"
#include "InterviewSystem.h"
#include "AssistantEngine.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Core/WorldContext.h"
#include "World/WUI/WuiJson.h"

#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define WE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define WE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
	using namespace World::Plugins;
	using namespace World::Assistant;

	constexpr const char* kPluginId = "engine.we-ai-assistant";
	const char* const kProvides[] = { "editor.panel", "llm.provider", "interview.system" };

	WeHostApi& Host()
	{
		static WeHostApi s_Host;
		return s_Host;
	}

	// 状态维护
	int g_ActiveTab = 0;
	std::string g_SelectedRole = "producer"; // producer | creative_director | tech_director | qa_director

	struct ChatEntry
	{
		std::string sender;
		std::string text;
		bool isUser = false;
	};
	std::vector<ChatEntry> g_ChatHistory = {
		{ "L1 制作人", "您好，出品人！我是您的 AI 执行制作人。在下方输入需求或点击快捷指令，我将组织各部门推进游戏制作。", false }
	};

	char g_InputBuffer[512] = "";
	std::string g_LastStatus = "系统就绪。";

	void SendUserMessage(const std::string& msg)
	{
		if (msg.empty()) return;
		g_ChatHistory.push_back({ "出品人 (您)", msg, true });
		if (g_ChatHistory.size() > 6)
		{
			g_ChatHistory.erase(g_ChatHistory.begin());
		}

		std::string rawJson = AssistantEngine::Chat(msg, g_SelectedRole);
		std::string replyText = rawJson;
		auto parsed = World::Wui::JsonValue::Parse(rawJson, nullptr);
		if (parsed && parsed->type == World::Wui::JsonValue::Type::Object)
		{
			for (const auto& kv : parsed->Object)
			{
				if (kv.first == "reply" && kv.second.type == World::Wui::JsonValue::Type::String)
				{
					replyText = kv.second.String;
					break;
				}
			}
		}

		std::string senderName = "L1 制作人";
		if (g_SelectedRole == "creative_director") senderName = "创意总监";
		else if (g_SelectedRole == "tech_director") senderName = "技术总监";
		else if (g_SelectedRole == "qa_director" || g_SelectedRole == "qa") senderName = "QA 质检总监";

		g_ChatHistory.push_back({ senderName, replyText, false });
		if (g_ChatHistory.size() > 6)
		{
			g_ChatHistory.erase(g_ChatHistory.begin());
		}
		g_LastStatus = "指令已执行并回显。";
	}

	int DrawAssistantPanel(void* /*userData*/, const WeEditorUiApi* ui, void* uiContext)
	{
		if (!ui || !uiContext || !ui->Label || !ui->Button || !ui->Separator)
			return 1;

		// 1. 顶部 Tab 导航
		static const char* kTabNames[] = { "💬 职能对话", "📋 制作人访谈", "🎨 品味档案" };
		if (ui->TabBar)
		{
			ui->TabBar(uiContext, "assistant_tabs", kTabNames, 3, &g_ActiveTab);
		}
		else
		{
			ui->Label(uiContext, "=== WorldEngine AI Assistant ===");
		}
		ui->Separator(uiContext);

		// ==================== Tab 0: 职能对话 ====================
		if (g_ActiveTab == 0)
		{
			// 角色选择指示
			std::string roleBar = "当前对话总监: " + g_SelectedRole + "  | 切换: ";
			if (ui->LabelColored)
				ui->LabelColored(uiContext, roleBar.c_str(), 4 /*Accent*/);
			else
				ui->Label(uiContext, roleBar.c_str());

			if (ui->Button(uiContext, "role_prod", "制作人")) g_SelectedRole = "producer";
			if (ui->Button(uiContext, "role_cd", "创意总监")) g_SelectedRole = "creative_director";
			if (ui->Button(uiContext, "role_td", "技术总监")) g_SelectedRole = "tech_director";
			if (ui->Button(uiContext, "role_qa", "QA总监")) g_SelectedRole = "qa_director";
			ui->Separator(uiContext);

			// 对话历史
			for (size_t i = 0; i < g_ChatHistory.size(); ++i)
			{
				const auto& item = g_ChatHistory[i];
				std::string line = (item.isUser ? "👤 " : "🤖 ") + item.sender + ": " + item.text;
				if (ui->LabelColored)
				{
					ui->LabelColored(uiContext, line.c_str(), item.isUser ? 4 /*Accent*/ : 0 /*Default*/);
				}
				else
				{
					ui->Label(uiContext, line.c_str());
				}
			}
			ui->Separator(uiContext);

			// 输入框与发送按钮
			bool submit = false;
			if (ui->TextInput)
			{
				submit = ui->TextInput(uiContext, "input_chat_msg", g_InputBuffer, sizeof(g_InputBuffer));
			}

			if (ui->Button(uiContext, "btn_send_chat", "发送指令 (Send)") || submit)
			{
				std::string msg(g_InputBuffer);
				if (!msg.empty())
				{
					SendUserMessage(msg);
					g_InputBuffer[0] = 0;
				}
			}

			// 快速指令 Chip 按钮
			if (ui->Button(uiContext, "chip_task", "⚡ 快速指令: 创建核心手感任务"))
			{
				SendUserMessage("创建任务: 打磨主角跳跃初速度与两段跳手感");
			}
			if (ui->Button(uiContext, "chip_gate", "🛡️ 快速指令: 运行项目门禁验证"))
			{
				SendUserMessage("运行门禁");
			}
		}
		// ==================== Tab 1: 制作人访谈 ====================
		else if (g_ActiveTab == 1)
		{
			if (ui->LabelColored)
				ui->LabelColored(uiContext, "【制作人 3 题访谈】点击选项自动沉淀至 taste.yaml 与 assumptions.md", 1 /*Success*/);
			else
				ui->Label(uiContext, "【制作人 3 题访谈】点击选项自动沉淀");

			auto round = InterviewSystem::Get().GetCurrentRound("", 0);
			for (size_t i = 0; i < round.questions.size(); ++i)
			{
				const auto& q = round.questions[i];
				std::string qHeader = "Q" + std::to_string(i + 1) + ": " + q.header + " (" + q.prompt + ")";
				ui->Label(uiContext, qHeader.c_str());

				for (const auto& opt : q.options)
				{
					std::string btnText = (opt.isRecommended ? "★ [推荐] " : "   ") + opt.label;
					std::string btnId = q.id + "_" + opt.id;
					if (ui->Button(uiContext, btnId.c_str(), btnText.c_str()))
					{
						InterviewSystem::Get().AnswerQuestion("", q.id, opt.id);
						g_LastStatus = "已选定 [" + q.header + "]: " + opt.label;
					}
				}
				ui->Separator(uiContext);
			}

			std::string statusLine = "状态: " + g_LastStatus;
			if (ui->LabelColored)
				ui->LabelColored(uiContext, statusLine.c_str(), 1 /*Success*/);
			else
				ui->Label(uiContext, statusLine.c_str());
		}
		// ==================== Tab 2: 品味档案 ====================
		else if (g_ActiveTab == 2)
		{
			if (ui->LabelColored)
				ui->LabelColored(uiContext, "=== 当前项目品味偏好档案 (taste.yaml) ===", 4 /*Accent*/);
			else
				ui->Label(uiContext, "=== 当前项目品味偏好档案 (taste.yaml) ===");

			std::string taste = InterviewSystem::Get().GetTasteProfile("");
			ui->Label(uiContext, taste.c_str());
			ui->Separator(uiContext);

			ui->Label(uiContext, "所有美术生成、策划文案、角色启闭均受上述档案统一约束。");
		}

		return 0;
	}

	bool Register(World::WorldContext&, const WeHostApi& host)
	{
		Host() = host;

		// 1. 注册命令到 CommandRegistry
		auto& reg = World::Ai::CommandRegistry::Get();
		InterviewSystem::RegisterCommands(reg);
		AssistantEngine::RegisterCommands(reg);

		// 2. 注册编辑器面板
		if (host.AbiVersion == WE_PLUGIN_ABI_VERSION
			&& host.StructSize >= offsetof(WeHostApi, UnregisterEditorPanel) + sizeof(host.UnregisterEditorPanel)
			&& host.RegisterEditorPanel)
		{
			WeEditorPanelDesc panel;
			panel.Id = "ai_assistant";
			panel.Title = "AI Assistant (AI 助手)";
			panel.Draw = &DrawAssistantPanel;
			host.RegisterEditorPanel(host.UserData, &panel);
		}

		return true;
	}

	void Unregister(World::WorldContext&)
	{
		const WeHostApi& host = Host();
		if (host.UserData && host.UnregisterEditorPanel)
		{
			host.UnregisterEditorPanel(host.UserData, "ai_assistant");
		}
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = kPluginId;
		plugin.Name = "WorldEngine AI Assistant";
		plugin.Version = "1.0.0";
		plugin.MinEngineVersion = ">=2.0";
		plugin.Provides = kProvides;
		plugin.ProvidesCount = 3;
		plugin.Register = &Register;
		plugin.Unregister = &Unregister;
		return plugin;
	}
}

WE_PLUGIN_EXPORT const WePlugin* WePluginQuery(uint32_t hostAbiVersion)
{
	if (hostAbiVersion != WE_PLUGIN_ABI_VERSION)
		return nullptr;
	static const WePlugin plugin = MakePlugin();
	return &plugin;
}
