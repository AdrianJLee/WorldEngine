#include "World/Plugins/WePluginApi.h"
#include "InterviewSystem.h"
#include "AssistantEngine.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Core/WorldContext.h"

#include <string>
#include <cstddef>

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

	std::string g_LastStatus = "AI Assistant ready. Choose an option or action below.";
	std::string g_LatestReply = "";

	int DrawAssistantPanel(void* /*userData*/, const WeEditorUiApi* ui, void* uiContext)
	{
		if (!ui || !uiContext || !ui->Label || !ui->Button || !ui->Separator)
			return 1;

		ui->Label(uiContext, "=== WorldEngine AI Assistant (L1 Producer) ===");
		ui->Separator(uiContext);

		ui->Label(uiContext, g_LastStatus.c_str());
		if (!g_LatestReply.empty())
		{
			ui->Label(uiContext, g_LatestReply.c_str());
		}
		ui->Separator(uiContext);

		// 制作人 3 题访谈卡片
		ui->Label(uiContext, "--- Producer 3-Question Interview Card (Concept) ---");
		auto round = InterviewSystem::Get().GetCurrentRound("", 0);
		for (size_t i = 0; i < round.questions.size(); ++i)
		{
			const auto& q = round.questions[i];
			std::string qHeader = "[" + q.header + "] " + q.prompt;
			ui->Label(uiContext, qHeader.c_str());

			for (const auto& opt : q.options)
			{
				std::string btnText = (opt.isRecommended ? "* [Rec] " : "  ") + opt.label;
				std::string btnId = q.id + "_" + opt.id;
				if (ui->Button(uiContext, btnId.c_str(), btnText.c_str()))
				{
					InterviewSystem::Get().AnswerQuestion("", q.id, opt.id);
					g_LastStatus = "Recorded [" + q.header + "]: " + opt.label + " -> Saved to taste.yaml & assumptions.md";
				}
			}
			ui->Separator(uiContext);
		}

		ui->Label(uiContext, "--- Fast Actions / Role Chat ---");
		if (ui->Button(uiContext, "chat_producer", "Chat with L1 Producer: Kickoff Concept"))
		{
			g_LatestReply = AssistantEngine::Chat("Kickoff Concept", "producer");
			g_LastStatus = "Instruction dispatched to L1 Producer.";
		}
		if (ui->Button(uiContext, "chat_qa", "Chat with QA Director: Verify Gates"))
		{
			g_LatestReply = AssistantEngine::Chat("Verify Gates", "qa");
			g_LastStatus = "Instruction dispatched to QA Director.";
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
