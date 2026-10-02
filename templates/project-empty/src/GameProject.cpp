#include "GameAPI.h"

// 项目层系统挂载入口。空模板里一个项目系统都没有,所以函数体是空的 —— 这就是"从零开始"
// 的起点:
//   1) 在 src/Systems/ 写一个 World::ISystem(一页教程见 src/Systems/README.md);
//   2) 在这里 #include "Systems/MySystem.h",Attach 里 scene.RegisterSystem<MySystem>(),
//      Detach 里 scene.UnregisterFrameSystem("MySystem")(字符串必须与 Name() 逐字相同);
//   3) 项目根构建:build.cmd → 编辑器 `文件 ▸ 重载 C++ 模块` → Play。
//
// 引擎在 Scene::OnRuntimeStart 末尾调 Attach、OnRuntimeStop 开头调 Detach:系统只活在一次
// 运行时内(Play 停止 → Detach,再 Play → Attach)。
namespace World::Game
{
	void AttachProjectSystems(Scene& scene)
	{
		(void)scene;
		// 目前没有项目系统。示例:
		// scene.RegisterSystem<MySystem>();
	}

	void DetachProjectSystems(Scene& scene)
	{
		(void)scene;
		// 与 Attach 一一对应。示例:
		// scene.UnregisterFrameSystem("MySystem");
	}
}
