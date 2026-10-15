#include "World/Plugins/WePluginApi.h"
#include "StudioPolicy.h"
#include "BacklogManager.h"
#include "StudioValidator.h"
#include "World/Agent/WeAgentCore.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Core/WorldContext.h"

#include <memory>

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

	bool Register(World::WorldContext& /*context*/, const WeHostApi& /*host*/)
	{
		// 1. 注册工作流策略到引擎编排内核
		auto policy = std::make_shared<StudioPolicy>("default");
		World::Agent::WeAgentCore::Get().SetPolicy(policy);

		// 2. 注册命令到 CommandRegistry
		auto& reg = World::Ai::CommandRegistry::Get();
		BacklogManager::RegisterCommands(reg);
		StudioValidator::RegisterCommands(reg);

		return true;
	}

	void Unregister(World::WorldContext& /*context*/)
	{
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
