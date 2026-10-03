#include "GameAPI.h"
#include "Systems/ExampleSystems.h"

// 项目层系统挂载入口(引擎在 Scene::OnRuntimeStart / OnRuntimeStop 里调用):
//   * AttachProjectSystems —— 每次进入运行时(编辑器 Play/Simulate、Runtime)把本项目的
//     C++ 系统挂到场景上;
//   * DetachProjectSystems —— 每次离开运行时把上面挂的系统摘掉。
// 系统只活在一次运行时内:停止 → Detach,再 Play → 重新 Attach。
//
// Attach 与 Detach 均支持类型模板:scene.RegisterSystem<T>() 与 scene.UnregisterSystem<T>()
// 完全对称且类型安全,避免手写字符串拼写错误。
namespace World::Game
{
	void AttachProjectSystems(Scene& scene)
	{
		scene.RegisterSystem<ExampleSpinSystem>();
		scene.RegisterSystem<ExampleEntityCountSystem>();
	}

	void DetachProjectSystems(Scene& scene)
	{
		scene.UnregisterSystem<ExampleSpinSystem>();
		scene.UnregisterSystem<ExampleEntityCountSystem>();
	}
}
