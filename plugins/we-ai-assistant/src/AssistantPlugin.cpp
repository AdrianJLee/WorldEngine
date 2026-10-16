#include "World/Plugins/WePluginApi.h"
#include "InterviewSystem.h"
#include "AssistantEngine.h"
#include "LlmClient.h"
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
	int g_SelectedRoleIdx = 0; // 0=producer, 1=creative_director, 2=tech_director, 3=qa_director

	struct ChatEntry
	{
		std::string sender;
		std::string text;
		bool isUser = false;
	};
	std::vector<ChatEntry> g_ChatHistory = {
		{ "L1 制作人", "您好，出品人！我是您的 AI 执行制作人。\n点击下方「🎮 一键做个游戏」或输入需求，我将立即为您建立立项工程并下发任务！", false }
	};

	char g_InputBuffer[512] = "";
	char g_EndpointBuffer[256] = "https://api.openai.com/v1/chat/completions";
	char g_ModelBuffer[128] = "gpt-4o";
	char g_ApiKeyBuffer[256] = "";

	std::string g_LastStatus = "系统就绪。";
	std::string g_TestPingResult = "未测试连接。";

	void SendUserMessage(const std::string& msg)
	{
		if (msg.empty()) return;
		g_ChatHistory.push_back({ "出品人 (您)", msg, true });
		if (g_ChatHistory.size() > 4)
		{
			g_ChatHistory.erase(g_ChatHistory.begin());
		}

		std::string roleName = "producer";
		if (g_SelectedRoleIdx == 1) roleName = "creative_director";
		else if (g_SelectedRoleIdx == 2) roleName = "tech_director";
		else if (g_SelectedRoleIdx == 3) roleName = "qa_director";

		std::string rawJson = AssistantEngine::Chat(msg, roleName);
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
		if (g_SelectedRoleIdx == 1) senderName = "创意总监";
		else if (g_SelectedRoleIdx == 2) senderName = "技术总监";
		else if (g_SelectedRoleIdx == 3) senderName = "QA 质检总监";

		g_ChatHistory.push_back({ senderName, replyText, false });
		if (g_ChatHistory.size() > 50)
		{
			g_ChatHistory.erase(g_ChatHistory.begin());
		}
		g_LastStatus = "已执行: " + msg;
	}

	int DrawAssistantPanel(void* /*userData*/, const WeEditorUiApi* ui, void* uiContext)
	{
		if (!ui || !uiContext || !ui->Label || !ui->Button || !ui->Separator)
			return 1;

		// 1. 顶部 Tab 导航 (4 标签: 对话 / 访谈 / 品味 / 模型设置)
		static const char* kTabNames[] = { "💬 职能对话", "📋 制作人访谈", "🎨 品味档案", "⚙️ 模型设置" };
		if (ui->TabBar)
		{
			ui->TabBar(uiContext, "assistant_tabs", kTabNames, 4, &g_ActiveTab);
		}
		else
		{
			ui->Label(uiContext, "=== WorldEngine AI Assistant ===");
		}
		ui->Separator(uiContext);

		// ==================== Tab 0: 职能对话 ====================
		if (g_ActiveTab == 0)
		{
			// 角色选择条
			static const char* kRoles[] = { "制作人", "创意总监", "技术总监", "QA总监" };
			if (ui->TabBar)
			{
				ui->TabBar(uiContext, "role_tab_bar", kRoles, 4, &g_SelectedRoleIdx);
			}

			// 输入框置顶
			bool submit = false;
			if (ui->TextInput)
			{
				submit = ui->TextInput(uiContext, "input_chat_msg", g_InputBuffer, sizeof(g_InputBuffer));
			}

			// 发送与动作按钮
			if (ui->Button(uiContext, "btn_send_chat", "🚀 发送指令 (Send)") || submit)
			{
				std::string msg(g_InputBuffer);
				if (!msg.empty())
				{
					SendUserMessage(msg);
					g_InputBuffer[0] = 0;
				}
			}

			if (ui->Button(uiContext, "btn_make_game_quick", "🎮 一键做个游戏 (Kickoff Project)"))
			{
				SendUserMessage("做个游戏");
			}

			if (ui->Button(uiContext, "btn_gate_quick", "🛡️ 运行项目门禁自检"))
			{
				SendUserMessage("运行门禁");
			}
			ui->Separator(uiContext);

			// 对话历史
			for (size_t i = 0; i < g_ChatHistory.size(); ++i)
			{
				const auto& item = g_ChatHistory[i];
				std::string header = (item.isUser ? "👤 " : "🤖 ") + item.sender + ":";
				if (ui->LabelColored)
				{
					ui->LabelColored(uiContext, header.c_str(), item.isUser ? 4 /*Accent*/ : 1 /*Success*/);
					ui->LabelColored(uiContext, item.text.c_str(), 0 /*Default*/);
				}
				else
				{
					ui->Label(uiContext, header.c_str());
					ui->Label(uiContext, item.text.c_str());
				}
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
		// ==================== Tab 3: 模型设置 ====================
		else if (g_ActiveTab == 3)
		{
			auto& llm = LlmClient::Get();

			if (ui->LabelColored)
				ui->LabelColored(uiContext, "=== LLM 大模型推理服务接入与凭据托管 ===", 4 /*Accent*/);
			else
				ui->Label(uiContext, "=== LLM 大模型推理服务接入与凭据托管 ===");

			// 预设快捷填入
			if (ui->Button(uiContext, "preset_openai", "快速预设: OpenAI (GPT-4o)"))
			{
				std::strncpy(g_EndpointBuffer, "https://api.openai.com/v1/chat/completions", sizeof(g_EndpointBuffer));
				std::strncpy(g_ModelBuffer, "gpt-4o", sizeof(g_ModelBuffer));
			}
			if (ui->Button(uiContext, "preset_deepseek", "快速预设: DeepSeek (deepseek-chat)"))
			{
				std::strncpy(g_EndpointBuffer, "https://api.deepseek.com/chat/completions", sizeof(g_EndpointBuffer));
				std::strncpy(g_ModelBuffer, "deepseek-chat", sizeof(g_ModelBuffer));
			}
			if (ui->Button(uiContext, "preset_ollama", "快速预设: 本地 Ollama (http://localhost:11434, 免Key)"))
			{
				std::strncpy(g_EndpointBuffer, "http://localhost:11434/v1/chat/completions", sizeof(g_EndpointBuffer));
				std::strncpy(g_ModelBuffer, "qwen2.5", sizeof(g_ModelBuffer));
			}
			ui->Separator(uiContext);

			ui->Label(uiContext, "API 端点 (Endpoint URL):");
			if (ui->TextInput)
			{
				ui->TextInput(uiContext, "input_endpoint", g_EndpointBuffer, sizeof(g_EndpointBuffer));
			}

			ui->Label(uiContext, "模型标识 (Model Identifier):");
			if (ui->TextInput)
			{
				ui->TextInput(uiContext, "input_model", g_ModelBuffer, sizeof(g_ModelBuffer));
			}

			ui->Label(uiContext, "API Key (输入后点保存，将通过 Windows DPAPI 强加密):");
			if (ui->TextInput)
			{
				ui->TextInput(uiContext, "input_api_key", g_ApiKeyBuffer, sizeof(g_ApiKeyBuffer));
			}

			if (ui->Button(uiContext, "btn_save_dpapi", "💾 保存 API Key (Windows DPAPI 加密托管)"))
			{
				std::string key(g_ApiKeyBuffer);
				if (!key.empty())
				{
					llm.SaveApiKeyToDpapi(key);
					g_ApiKeyBuffer[0] = 0;
					g_TestPingResult = "API Key 已成功通过 Windows DPAPI 加密保存到 local/credentials/llm_key.bin！";
				}
			}

			// 同步配置并测试
			if (ui->Button(uiContext, "btn_test_ping", "🔌 测试模型连通性 (Ping Test)"))
			{
				LlmConfig cfg = llm.GetConfig();
				cfg.endpoint = g_EndpointBuffer;
				cfg.model = g_ModelBuffer;
				llm.SetConfig(cfg);

				std::vector<LlmMessage> testMsgs = {
					{ "system", "You are a test ping agent." },
					{ "user", "Ping. Reply exactly 'PONG' and your model name in 10 words." }
				};

				LlmResult res = llm.Chat(testMsgs);
				if (res.success)
				{
					g_TestPingResult = "✅ 连接成功！模型应答: " + res.content;
				}
				else
				{
					g_TestPingResult = "❌ 连接失败: " + res.error;
				}
			}

			if (ui->Button(uiContext, "btn_clear_key", "🗑️ 清除 DPAPI 已存密钥"))
			{
				llm.DeleteDpapiApiKey();
				g_TestPingResult = "已删除本地 DPAPI 加密密钥。";
			}

			ui->Separator(uiContext);
			std::string keyStatus = llm.HasDpapiApiKey() ? "🟢 DPAPI 密钥状态: 已保存加密凭据" : "⚪ DPAPI 密钥状态: 未配置 (运行在离线规则/本地模式)";
			if (ui->LabelColored)
			{
				ui->LabelColored(uiContext, keyStatus.c_str(), llm.HasDpapiApiKey() ? 1 /*Success*/ : 2 /*Warning*/);
				ui->LabelColored(uiContext, g_TestPingResult.c_str(), 0 /*Default*/);
			}
			else
			{
				ui->Label(uiContext, keyStatus.c_str());
				ui->Label(uiContext, g_TestPingResult.c_str());
			}
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

