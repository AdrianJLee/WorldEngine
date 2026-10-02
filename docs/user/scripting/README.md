# 脚本

引擎内嵌 **Luau**。纯 ECS 架构下,脚本**不挂在实体上** —— 用脚本写**系统(System)**,
由场景的帧管线统一驱动。

一句话分层:

- **组件**是纯数据,用 **C++** 的 `WE_SCHEMA_BODY` 定义（见 [扩展引擎](../projects/README.md)）。
- **逻辑**写成**系统**:C++ 派生 `World::ISystem`,或 Luau 写成系统脚本。
- **Luau 不能定义组件类型** —— 类型与字段只有 C++ schema 一份。

## 快速开始（Luau）

1. 在项目内容树的 `<内容根>/scripts/systems/` 下新建 `.luau` 文件（目录不存在就建一个）。
2. 脚本里用 `ecs:Query` 建查询、`ecs:AddSystem` 把系统挂进管线。
3. `Play`:场景启动时整份执行一次,之后每帧调用你注册的更新函数。

```lua
-- <内容根>/scripts/systems/player_movement.luau
local movement = ecs:Query({ "TransformComponent", "VelocityComponent" })

ecs:AddSystem("PlayerMovementSystem", function(dt)
    movement:Each(function(entity, transform, velocity)
        transform.Location.x = transform.Location.x + velocity.Linear.x * dt
        transform.Location.y = transform.Location.y + velocity.Linear.y * dt
        transform.Location.z = transform.Location.z + velocity.Linear.z * dt
    end)
end, "Update")
```

- 脚本可以直接是"脚本本体",不需要返回 table;`return` 一个 table 也没有副作用。
- `ecs` 这个全局表也可写成 `world`,两者是同一张表。

## `ecs` API

| 调用 | 作用 |
| --- | --- |
| `ecs:Query({ "CompA", "CompB" })` | 建查询,结果有 `:Each(fn)` 与 `:Count()` |
| `ecs:Query({ "CompA" }, { without = { "DeadTag" } })` | 排除过滤:带 `without` 里任一组件的实体不参与 |
| `ecs:AddSystem(name, fn [, phase])` | 注册具名系统;同名先撤销再注册（**幂等**） |
| `ecs:AddSystem(name, fn, { phase = "Late", after = { "Other" } })` | 指定阶段 + 同阶段顺序依赖 |
| `ecs:RemoveSystem(name)` | 撤销系统,不存在返回 `false` |
| `ecs:CreateEntity([name])` | 建实体 |
| `ecs:DestroyEntity(entity)` | 排队销毁(下一个安全点提交) |
| `ecs:EntityCount()` | 当前存活实体数 |
| `ecs:OnAdd(comp, fn)` / `ecs:OnRemove(comp, fn)` | 观察组件增删,返回句柄 |
| `ecs:Off(handle)` | 取消观察者 |

`:Each` 的回调签名是 `(entity, 组件代理...)`,组件代理按 `Query` 里列出的组件顺序传入,
可以直接读写字段（例如 `transform.Location.x = ...`）。

**阶段**:`phase` 省略时是 `"Update"`。完整顺序是 `PreFixed → Fixed → Update → Late → PreRender`
—— 引擎**每帧按这五个阶段依次跑一遍**,所以写在非 `Update` 阶段的系统真的在那一刻执行。

**顺序依赖**:`after = { "OtherSystem" }` 表示"本系统排在 `OtherSystem` 之后"(同阶段内生效)。
依赖的系统不存在时会**跳过本系统并记一条警告**,不会静默乱序 —— 名字要与对方 `AddSystem`
的第一个参数逐字一致。

**排除过滤**:`without` 里写"有这个组件就不要"的组件名;想表达"有 A 没有 B"就写
`ecs:Query({ "A" }, { without = { "B" } })`。选项表里出现未知键、或 `without` 里写了
未注册的组件名,都会报可读错误(不静默忽略)。

**幂等**:`AddSystem` 对同名系统先撤销再注册,所以"同一场景二次启动"或"系统脚本热重载"
都是整份重跑,不会因为重名报错。

## 响应式逻辑（替代旧的 `OnCreate` / `OnDestroy`）

需要"某个组件出现/消失时做一次事"时,用观察者,不要轮询:

```lua
ecs:OnAdd("DeadTag", function(entity)
    print("实体死亡:", entity:GetName())
end)

local handle = ecs:OnRemove("FrozenTag", function(entity)
    -- 解冻
end)
ecs:Off(handle)   -- 不需要了就注销
```

## 热重载

- 改 `<内容根>/scripts/systems/` 下的脚本,**保存即生效**:编辑器每帧轮询,内容变化后
  150ms 消抖,然后**整份重跑**该系统脚本（先撤销它上次注册的系统,再重新执行）。
- 只改数值、改函数体都生效;改回原内容不算变化。
- 场景停止时,系统脚本注册的系统会被整体撤销,下次 `Play` 重新装载。
- 发行形态（`Runtime.exe` + 打包内容）没有监听器;详见 [热重载生效矩阵](../../dev/hot-reload.md)。

## 代码提示

- 编辑器按实际注册的绑定生成 `<内容根>/assets/scripts/intermediate/WorldEngineAPI.luau`;
  里面包含 `Entity`、各组件类、`ecs` 表、`events`/`timers`/`ui` 等全局表的声明。
  **不要** `require` 或运行它。
- VS Code 打开**项目根**即可:项目里的 `.vscode/settings.json` 与 `.luau-lsp/config.json`
  （新建项目时生成）给 Luau LSP 喂同一份声明。
- 细节见 [Lua 脚本与代码提示](lua-tooling.md)。

## 已知边界

- **`ui.*` 暂时无效**。旧的 `OnUI` 生命周期随单实体脚本一起删除,脚本 UI 的表达方式还在讨论中;
  现在脚本里调 `ui.*` 不会报错,但也不会有任何显示效果。**不要依赖它。**
- **Luau 不能新增组件类型**。需要新数据就在 C++ 侧加组件（见 `docs/dev/scripting-architecture.md` §1 的理由）。
- 结构修改（建/删实体）在系统回调里是**排队**的,下一个安全点提交;不要在回调里假设"下一行就生效"。

## 架构细节

数据/逻辑/脚本的完整边界、以及为什么 Luau 不能定义组件,见
[脚本与系统架构（纯 ECS）](../../dev/scripting-architecture.md)。
