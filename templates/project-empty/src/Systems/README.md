# src/Systems/ — 怎么加第一个系统

这个目录放项目的 C++ 逻辑(纯 ECS 的 System)。Luau 逻辑放在
`assets/scripts/systems/*.luau`,不放这里。

## 五步

1. 写一个系统(`src/Systems/MySystem.h`):

```cpp
#pragma once

#include "World/Core/Timestep.h"
#include "World/Scene/Components.h"
#include "World/Scene/ISystem.h"
#include "World/Scene/Query.h"
#include "World/Scene/Scene.h"
#include "World/Scene/TransformSystem.h"

#include <string_view>

namespace World::Game
{
	class MySystem : public ISystem
	{
	public:
		std::string_view Name() const override { return "MySystem"; }
		Gameplay::SystemPhase Phase() const override { return Gameplay::SystemPhase::Update; }
		bool ParallelSafe() const override { return false; }

		void Update(Scene& scene, Timestep dt) override
		{
			const float delta = dt.GetSeconds();
			scene.Query<TransformComponent, const VelocityComponent>()
				.Each([delta](TransformComponent& transform, const VelocityComponent& velocity) {
					transform.Location += velocity.Linear * delta;
					TransformSystem::Recalculate(transform);
				});
		}
	};
}
```

2. 在 `src/GameProject.cpp` 顶部 `#include "Systems/MySystem.h"`,再在 `AttachProjectSystems` 里挂上:

```cpp
void AttachProjectSystems(Scene& scene) { scene.RegisterSystem<MySystem>(); }
```

3. 在 `DetachProjectSystems` 里摘掉(**字符串与 `Name()` 逐字相同**):

```cpp
void DetachProjectSystems(Scene& scene) { scene.UnregisterFrameSystem("MySystem"); }
```

4. 在项目根构建:`build.cmd`
5. 回到编辑器执行 `文件 ▸ 重载 C++ 模块`,然后 Play —— 系统在运行时挂上,停止时摘掉。

完整可跑的例子(两个系统 + 逐行注释)见示例项目模板的 `src/Systems/ExampleSystems.h`;
引擎侧的参考实现是 `Engine/src/World/Scene/MovementSystem.{h,cpp}`。
