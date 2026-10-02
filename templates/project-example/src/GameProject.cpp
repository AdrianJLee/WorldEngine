#include "GameAPI.h"
#include "Systems/ExampleSystems.h"

// 项目层系统挂载入口(引擎在 Scene::OnRuntimeStart / OnRuntimeStop 里调用):
//   * AttachProjectSystems —— 每次进入运行时(编辑器 Play/Simulate、Runtime)把本项目的
//     C++ 系统挂到场景上;
//   * DetachProjectSystems —— 每次离开运行时把上面挂的系统摘掉。
// 系统只活在一次运行时内:停止 → Detach,再 Play → 重新 Attach。
//
// 契约:Detach 里的字符串必须与系统 Name() 的返回值**逐字相同**(大小写、拼写都不能差),
// 否则 UnregisterFrameSystem 找不到该系统、会静默留下一个跨运行存活的对象。
namespace World::Game
{
	void AttachProjectSystems(Scene& scene)
	{
		scene.RegisterSystem<ExampleSpinSystem>();
		scene.RegisterSystem<ExampleEntityCountSystem>();
	}

	void DetachProjectSystems(Scene& scene)
	{
		scene.UnregisterFrameSystem("ExampleSpinSystem");
		scene.UnregisterFrameSystem("ExampleEntityCountSystem");
	}
}
