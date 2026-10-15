#include "World/Plugins/WePluginApi.h"
#include "StudioPolicy.h"
#include "BacklogManager.h"
#include "StudioValidator.h"
#include "World/Agent/WeAgentCore.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Core/WorldContext.h"

#include <memory>
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
	using namespace World::Studio;

	constexpr const char* kPluginId = "engine.we-studio";
	const char* const kProvides[] = { "workflow.policy", "workflow.backlog", "editor.ui" };

	WeHostApi& Host()
	{
		static WeHostApi s_Host;
		return s_Host;
	}

	std::string g_StudioStatus = "Studio workflow active. Gates and roles ready.";

	int DrawStudioPanel(void* /*userData*/, const WeEditorUiApi* ui, void* uiContext)
	{
		if (!ui || !uiContext || !ui->Label || !ui->Button || !ui->Separator)
			return 1;

		ui->Label(uiContext, "=== WorldEngine Studio Workflow ===");
		ui->Separator(uiContext);

		ui->Label(uiContext, "Preset: default (40 Roles / 8-Stage Pipeline)");
		ui->Label(uiContext, "Backlog: .we/studio/backlog/ | Bugs: .we/studio/bugs/");
		ui->Separator(uiContext);

		ui->Label(uiContext, g_StudioStatus.c_str());

		if (ui->Button(uiContext, "btn_validate", "Run Studio Validation (studio.validate)"))
		{
			std::string res, err;
			auto& reg = World::Ai::CommandRegistry::Get();
			if (reg.Dispatch("studio.validate", {}, res, err))
			{
				g_StudioStatus = "Validation OK: " + res;
			}
			else
			{
				g_StudioStatus = "Validation Failed: " + err;
			}
		}

		return 0;
	}

	bool Register(World::WorldContext& /*context*/, const WeHostApi& host)
	{
		Host() = host;

		// 1. 注册工作流策略到引擎编排内核
		auto policy = std::make_shared<StudioPolicy>("default");
		World::Agent::WeAgentCore::Get().SetPolicy(policy);

		// 2. 注册命令到 CommandRegistry
		auto& reg = World::Ai::CommandRegistry::Get();
		BacklogManager::RegisterCommands(reg);
		StudioValidator::RegisterCommands(reg);

		// 3. 注册编辑器面板
		if (host.AbiVersion == WE_PLUGIN_ABI_VERSION
			&& host.StructSize >= offsetof(WeHostApi, UnregisterEditorPanel) + sizeof(host.UnregisterEditorPanel)
			&& host.RegisterEditorPanel)
		{
			WeEditorPanelDesc panel;
			panel.Id = "studio_workflow";
			panel.Title = "Studio Workflow (工作流看板)";
			panel.Draw = &DrawStudioPanel;
			host.RegisterEditorPanel(host.UserData, &panel);
		}

		return true;
	}

	void Unregister(World::WorldContext& /*context*/)
	{
		const WeHostApi& host = Host();
		if (host.UserData && host.UnregisterEditorPanel)
		{
			host.UnregisterEditorPanel(host.UserData, "studio_workflow");
		}

		// 复位为最小后备策略
		World::Agent::WeAgentCore::Get().ResetToMinimalPolicy();
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = kPluginId;
		plugin.Name = "WorldEngine Studio";
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
