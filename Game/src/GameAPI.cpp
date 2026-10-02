#include "GameAPI.h"
#include "Generated/Game/GameSchemaRegistration.h"

namespace
{
	bool RegisterGameModule(World::WorldContext& context)
	{
		if (!World::Schema::RegisterGameSchemaModule(context.Schemas()))
			return false;
		// PURE-ECS:项目层 C++ 系统的挂载入口(项目实现 src/GameProject.cpp;缺席时空实现)。
		context.AddSceneSystemsHook(&World::Game::AttachProjectSystems, &World::Game::DetachProjectSystems);
		return true;
	}

	void UnregisterGameModule(World::WorldContext& context)
	{
		context.RemoveSceneSystemsHook(&World::Game::AttachProjectSystems, &World::Game::DetachProjectSystems);
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

#if defined(WLD_GAME_NO_PROJECT_HOOK)
// 项目没有提供 `<项目根>/src/GameProject.cpp` 时的空实现(构建期探测,见 Game/CMakeLists.txt):
// 引擎骨架构建与"零系统项目"都走这一支,不会因为少一个文件而链接失败。
namespace World::Game
{
	void AttachProjectSystems(Scene&) {}
	void DetachProjectSystems(Scene&) {}
}
#endif
