# 写一个系统并让它跑起来(最小例子)

> 面向**做游戏的人**。概念与"怎么只管某些实体"见 [实体与系统](entities-and-systems.md)。

## 最短路径(Luau,不用编译)

### 1. 建系统脚本

`文件 ▸ 新建 Lua …` → 类型选 **系统** → 名字 `Mover`
(等价于手动在 `<内容根>/scripts/systems/mover.luau` 建文件)。

### 2. 写逻辑

```lua
-- <内容根>/scripts/systems/mover.luau
local movers = ecs:Query({ "TransformComponent", "VelocityComponent" })

ecs:AddSystem("MoverSystem", function(dt)
    movers:Each(function(entity, transform, velocity)
        transform.Location = transform.Location + velocity.Linear * dt
    end)
end, "Update")
```

### 3. **给实体挂上它查的组件**(最常忘的一步)

查询就是系统的**作用范围**:上面这个系统只会碰"同时有 `TransformComponent` 和
`VelocityComponent`"的实体。**没有实体带 `VelocityComponent` ⇒ 系统每帧跑了但什么也没做。**

在编辑器里:选中实体 → 属性面板 `添加组件` → 找到 `VelocityComponent`,把 `Linear` 填成
`1, 0, 0`。

(.wd 场景文件里长这样 —— 组件就是一个块:)

```yaml
  - World::UUIDComponent:
      ID: { m_UUID: 1234 }
    World::TagComponent:
      Tag: Mover
    World::TransformComponent:
      Location: [0, 0, 0]
    World::VelocityComponent:      # ← 系统靠这一块认出它
      Linear: [1, 0, 0]
```

### 4. Play

按 `播放`。系统脚本在场景启动时被自动加载(日志里有一行
`[Luau System Loader] 'scripts/systems/mover.luau' -> 1 system(s)`),
之后每帧调用你注册的函数。

**实测**(速度 `Linear = 1,0,0`):

```
Play 前 : [0, 0, 0]
Play 1s : [1.01308, 0, 0]
Play 2s : [2.01988, 0, 0]
Play 3s : [3.03259, 0, 0]
```

停止后场景文档**没有**被改动(仍是 `[0,0,0]`)—— 运行时改的是场景副本,不写回文件。

## C++ 版本(要编译)

### 1. 建系统头文件

`文件 ▸ 新建 C++ …` → 类型选 **系统** → 名字 `PushForwardSystem`。
向导会生成 `<项目根>/src/Systems/PushForwardSystem.h`,**并且自动**把两行登记写进
`<项目根>/src/GameProject.cpp`。

**如果你是手写的**这个头文件(没用向导),那两行要自己加:

```cpp
// <项目根>/src/Systems/PushForwardSystem.h
#pragma once
#include "World/Core/Timestep.h"
#include "World/Scene/Components.h"
#include "World/Scene/ISystem.h"
#include "World/Scene/Query.h"
#include "World/Scene/Scene.h"
#include <string_view>

namespace World::Game
{
	class PushForwardSystem : public ISystem
	{
	public:
		std::string_view Name() const override { return "PushForwardSystem"; }
		Gameplay::SystemPhase Phase() const override { return Gameplay::SystemPhase::Update; }
		bool ParallelSafe() const override { return false; }

		void Update(Scene& scene, Timestep dt) override
		{
			const float delta = dt.GetSeconds();
			scene.Query<TransformComponent, const VelocityComponent>()
				.Each([delta](TransformComponent& transform, const VelocityComponent& velocity) {
					transform.Location += velocity.Linear * delta;
					transform.RecalculateTransform();
				});
		}
	};
}
```

```cpp
// <项目根>/src/GameProject.cpp —— 手写系统必须自己加这两处(名字必须逐字相同)
#include "GameAPI.h"
#include "Systems/PushForwardSystem.h"      // ← 头文件是 header-only,不 include 就进不了 Game.dll

namespace World::Game
{
	void AttachProjectSystems(Scene& scene)
	{
		scene.RegisterSystem<PushForwardSystem>();                 // ← 挂上
	}

	void DetachProjectSystems(Scene& scene)
	{
		scene.UnregisterFrameSystem("PushForwardSystem");          // ← 摘掉(与 Name() 逐字相同)
	}
}
```

### 2. 编译 + 加载

项目根跑 `build.cmd`,然后回到编辑器 `文件 ▸ 构建并重载 C++ 模块(Game.dll)`
(这一条会替你跑 `build.cmd`)。

### 3. Play —— 同 Luau 版的第 3、4 步

(同样要给实体挂 `VelocityComponent`,否则系统什么也不做。)

**实测**(手写 + 手工登记,`Linear = 1,0,0`):

```
Play 前 : [0, 0, 0]
Play 2s : [4.06359, 0, 0]     ← 方块在动;秒数含取快照的往返时间
```

## 怎么确认"我的系统真的在跑"

| 手段 | 看什么 |
| --- | --- |
| **Systems Pipeline 面板**(`窗口 ▸ Systems Pipeline`) | 每个系统一行 + 每帧耗时。**你的系统名出现在里面 = 已注册并在被调度** |
| Play 停不下来/没效果 | 先看上面那张表里有没有你的系统;没有 ⇒ 名字没登记对(Luau:文件不在 `scripts/systems/`;C++:`GameProject.cpp` 少了一行,或没重载模块) |
| 日志 | Luau:`[Luau System Loader] '…' -> N system(s)`,`N=0` 说明脚本跑了但没 `AddSystem` |
| 系统里 `print(...)` | 脚本输出会进编辑器日志(默认 info 级就可见) |

## 三个最常踩的坑

1. **没给实体挂组件** ⇒ 查询命中 0 个,系统静默什么也不做。
   (这就是"系统怎么附加到实体"的答案:**你不用附加,给实体挂上它查的组件就行**。)
2. **Luau 脚本放错目录** ⇒ 只有 `<内容根>/scripts/systems/` 下的会被自动加载;
   放 `scripts/` 根或别处都不会跑。
3. **C++ 改了没重载** ⇒ 头文件不 include 进 `GameProject.cpp`(或忘了
   `构建并重载 C++ 模块`),改动不会生效。
