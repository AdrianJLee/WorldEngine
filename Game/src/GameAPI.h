#pragma once

#include "World/Core/WorldContext.h"
#include "World/Modules/WeModule.h"

namespace World
{
	class Scene;
}

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

namespace World::Game
{
	// ---- PURE-ECS:项目层系统挂载入口 ----
	// 项目在自己的源码里实现这两个函数(模板 = `<项目根>/src/GameProject.cpp`),Game 模块把
	// 它们登记进 `WorldContext` 的场景钩子:每次场景进入/离开运行时各调用一次
	// (编辑器 Play / Simulate、独立 Runtime 与 Game 自带宿主同一条路径)。
	//
	//   AttachProjectSystems —— 在这里把本项目的 C++ 系统挂上场景:
	//       scene.RegisterSystem<MyMovementSystem>();
	//   DetachProjectSystems —— 撤销上面挂的系统(名字与 Attach 一一对应):
	//       scene.UnregisterFrameSystem("MyMovementSystem");
	//
	// **系统只活在一次运行时内**:Play 停止 → Detach,再次 Play → Attach(与 Lua 系统脚本
	// 同一生命周期)。组件(纯数据)在 `<项目根>/src/Components/*.h` 里用 WE_SCHEMA_BODY 声明,
	// 由 schema 生成编进本模块的注册表 —— 两者都不需要动引擎源码。
	//
	// 项目没有提供 `src/GameProject.cpp` 时,Game 模块编一份空实现(见 GameAPI.cpp),
	// 构建不会因为少一个文件而失败。
	void AttachProjectSystems(Scene& scene);
	void DetachProjectSystems(Scene& scene);
}
