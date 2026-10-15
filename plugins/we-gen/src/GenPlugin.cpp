#include "World/Plugins/WePluginApi.h"
#include "GenPipeline.h"
#include "World/Ai/CommandRegistry.h"
#include "World/Core/WorldContext.h"

#if defined(_WIN32)
#  define WE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define WE_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
	using namespace World::Plugins;
	using namespace World::Gen;

	constexpr const char* kPluginId = "engine.we-gen";
	const char* const kProvides[] = { "asset.generator", "credential.manager", "style.lock" };

	bool Register(World::WorldContext&, const WeHostApi&)
	{
		auto& reg = World::Ai::CommandRegistry::Get();
		GenPipeline::RegisterCommands(reg);
		return true;
	}

	void Unregister(World::WorldContext&)
	{
	}

	WePlugin MakePlugin()
	{
		WePlugin plugin;
		plugin.Id = kPluginId;
		plugin.Name = "WorldEngine Gen";
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
