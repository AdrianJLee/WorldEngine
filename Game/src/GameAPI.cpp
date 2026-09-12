#include "GameAPI.h"
#include "Generated/Game/GameSchemaRegistration.h"

namespace
{
	bool RegisterGameModule(World::WorldContext& context)
	{
		return World::Schema::RegisterGameSchemaModule(context.Schemas());
	}

	void UnregisterGameModule(World::WorldContext& context)
	{
		World::Schema::UnregisterGameSchemaModule(context.Schemas());
	}

	const World::Modules::WeModule kGameModule = {
		sizeof(World::Modules::WeModule),
		World::Modules::WE_MODULE_ABI_VERSION,
		"game",
		"WorldEngine Game Module",
		1,
		&RegisterGameModule,
		&UnregisterGameModule,
	};
}

extern "C" GAME_API const World::Modules::WeModule* WeGameModuleQuery(uint32_t hostAbiVersion)
{
	if (hostAbiVersion < World::Modules::WE_MODULE_ABI_VERSION)
		return nullptr;
	return &kGameModule;
}
