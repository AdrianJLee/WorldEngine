#pragma once

#include "World/Core/WorldContext.h"
#include "World/Modules/WeModule.h"

#ifdef _MSC_VER
#ifdef GAME_BUILD_DLL
#define GAME_API __declspec(dllexport)
#else
#define GAME_API __declspec(dllimport)
#endif
#else
#define GAME_API
#endif

// Game 模块入口:宿主经此查询模块描述并完成显式注册;Game.dll 内不存在注册表单例。
extern "C" GAME_API const World::Modules::WeModule* WeGameModuleQuery(uint32_t hostAbiVersion);
