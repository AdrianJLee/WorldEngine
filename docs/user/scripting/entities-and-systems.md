# 实体与系统:从"给实体挂脚本"迁移过来

> 面向**做游戏的人**。纯 ECS 架构下"逻辑放哪、怎么只作用在某些实体上"的唯一口径。
> 引擎内部边界见 [`docs/dev/scripting-architecture.md`](../../dev/scripting-architecture.md)。

## 一句话:系统**不挂在实体上**

| | 是什么 | 活在哪 |
| --- | --- | --- |
| **实体 (Entity)** | 一个 id + 它身上挂的一组组件 | 场景 |
| **组件 (Component)** | 纯数据(字段)。**没有逻辑** | 挂在实体上 |
| **系统 (System)** | 每帧遍历"符合查询的实体"的逻辑。**不挂在实体上** | 挂在**场景**上,由帧管线驱动 |

所以没有"把系统附加到实体"这一步 —— 系统**不是**实体的一部分。它每帧自己去问
"哪些实体符合我的查询条件",然后处理它们。

**一句话记法**:实体回答"要处理什么数据",系统回答"数据怎么变"。把两者绑起来的是
**查询条件**,不是挂载。

## 那"我只想让这个系统管某些实体"怎么办?

这正是**查询**要解决的问题。三种惯用法:

### 1) 用组件本身当条件(最常用)

系统查什么,就只管什么:

```lua
-- 只管"有速度"的实体 —— 没有 VelocityComponent 的实体不会进来
local movers = ecs:Query({ "TransformComponent", "VelocityComponent" })
ecs:AddSystem("Move", function(dt)
    movers:Each(function(entity, transform, velocity)
        transform.Location = transform.Location + velocity.Linear * dt
    end)
end)
```

C++ 同理:`scene.Query<TransformComponent, const VelocityComponent>().Each(...)`。

### 2) 用**标签组件**当开关(替代旧的"给实体挂脚本")

标签 = 零字段的组件,专门当过滤条件用。先声明一次:

```cpp
// <项目根>/src/Components/Tags.h
struct BurningTag {};
WE_SCHEMA_BODY(Game, BurningTag, Component)
```

然后两个系统共享同一份数据、按标签分工:

```lua
local burning = ecs:Query({ "BurningTag", "HealthComponent" })
ecs:AddSystem("BurnTick", function(dt)          -- 只有带标签的会烧
    burning:Each(function(entity, health) health.Value = health.Value - 2.0 * dt end)
end)

local allHealth = ecs:Query({ "HealthComponent" })   -- 不带标签的也能被另一个系统处理
```

**"让这个实体开始/停止用某个系统"** = 加/删那个标签组件:

```lua
entity:AddComponent("BurningTag")       -- 开始
entity:RemoveComponent("BurningTag")    -- 停止
```

这等价于旧写法里"给实体挂/摘脚本",但数据与逻辑各自在自己的地方。

### 3) 用 `without` 排除(`ecs:Query` 的第二参数)

想表达"有 A 但**没有** B"时用它,等价于 C++ 的 `Query<...>().Without<...>()`:

```lua
local alive = ecs:Query({ "HealthComponent" }, { without = { "DeadTag" } })
```

`without` 里写未注册的组件名会报可读错误(不会静默跳过)。

### 4) 参数化的行为:参数放**组件字段**,不放系统

"这个实体的移动速度是 3、那个是 7" —— 速度是**数据**,所以它是组件字段,不是系统配置:

```cpp
struct MoverComponent { float Speed = 1.0f; };   // 每个实体一份
```

系统读字段:

```lua
local movers = ecs:Query({ "MoverComponent", "TransformComponent" })
ecs:AddSystem("Move", function(dt)
    movers:Each(function(entity, mover, transform)
        transform.Location.x = transform.Location.x + mover.Speed * dt
    end)
end)
```

引擎自带的 `VelocityComponent` 就是这个模式(线性/角速度放在组件里)。

## 旧写法 → 新写法对照

| 你以前会写的 | 现在怎么写 |
| --- | --- |
| 给实体挂一个脚本,`OnCreate` 里初始化 | 系统里用 `ecs:OnAdd("SomeComponent", function(entity) … end)` |
| `OnUpdate` 里改自己 | `ecs:AddSystem` + 查询 `:Each` |
| `OnDestroy` 里收尾 | `ecs:OnRemove("SomeComponent", …)`,或系统里扫"待销毁"标签 |
| "这个脚本只管某些实体" | 给它挂**标签组件**,系统查这个标签 |
| "每个实体不同的行为参数" | 参数放**组件字段**,系统读字段 |
| 运行中"打开/关闭"某个行为 | 加/删那个标签组件(`AddComponent` / `RemoveComponent`) |

## 物理事件:接触与触发器

碰撞与传感器重叠也是"每帧一次的事实",用 `ecs:OnContact` / `ecs:OnTrigger` 订阅。
**在固定步长阶段**,物理步进把本帧的接触/重叠写进场景队列;**在可变阶段**(Update /
Late / PreRender),你的回调被逐条调用 —— 所以看到的是**本帧**的事件,与帧率无关。

```lua
-- <内容根>/scripts/systems/contact_fx.luau

-- 接触:两个实体的碰撞(有碰撞响应)
local contactHandle = ecs:OnContact(function(event)
    -- event.a / event.b 是两个 Entity(与查询、OnAdd 给的实体同一种)
    -- event.phase: "Begin" | "Persist" | "End"
    -- event.point / event.normal: { x = …, y = …, z = … }
    -- event.depth: 穿透深度(number)
    if event.phase == "Begin" then
        print("碰到一起:", event.a, event.b)
    end
end)

-- 触发器:传感器重叠(只上报事实,不产生碰撞响应)
local triggerHandle = ecs:OnTrigger(function(event)
    -- event.sensor: 挂传感器的实体;event.other: 与它重叠的实体
    print("trigger", event.sensor, event.other, event.phase)
end)

-- 不需要了就注销(和组件观察者共用同一个 ecs:Off)
-- ecs:Off(contactHandle)
-- ecs:Off(triggerHandle)
```

要点:

- **每帧一次、在可变阶段读**:一帧里固定步可能跑 0..N 次,但事件队列**每帧恰好清空一次**,
  回调拿到的永远是本帧产出的事件。系统脚本加载时注册即可,不用自己轮询。
- **`phase` 三种值**:`"Begin"`(本帧开始接触/进入)、`"Persist"`(持续接触)、`"End"`(本帧分离)。
  `"End"` 时几何量一律为零(`point = { x = 0, y = 0, z = 0 }`、`depth = 0`)。
- **接触 ≠ 触发**:`OnContact` 收的是实体接触(会挡住彼此);`OnTrigger` 收的是**传感器**
  (2D 在 BoxCollider2D / CircleCollider2D 上,3D 在 RigidBody3D 上)的重叠,不产生任何碰撞响应
  —— 穿过就穿过了。
- **2D 与 3D 的语义差异**:2D 的传感器是**逐 shape**,3D 的传感器是**逐刚体**(Jolt 原生模型);
  `Begin` / `Persist` / `End` 两侧都提供。
- **顺序与隔离**:同一帧按事件在队列里的顺序派发(确定性);某个回调报错只记一条日志,
  不会打断其它订阅者,也不会打断帧。
- **句柄生命周期**与 `ecs:OnAdd` / `ecs:OnRemove` 一致:只有 `ecs:Off(handle)` 显式注销。
  系统脚本热重载是"整份重跑",重跑时再次注册就会多一条订阅(与 `OnAdd` 同一口径),
  所以在脚本里注册一次就好,不要每帧重复注册。

## 系统自己的生命周期

系统活在一次**运行时**内(编辑器 Play/Simulate、独立 Runtime 各一次):

- **Luau 系统脚本**:放进 `<内容根>/scripts/systems/*.luau`,场景启动时**整份执行一次**
  (执行时 `ecs:AddSystem` 把系统注册进帧管线);停止时引擎自动撤销它们注册的系统。
  改动文件会热重载(整份重跑)。
- **C++ 系统**:在 `<项目根>/src/GameProject.cpp` 的 `AttachProjectSystems` 里
  `scene.RegisterSystem<MySystem>()`、`DetachProjectSystems` 里
  `scene.UnregisterFrameSystem("MySystem")`(名字逐字相同)。
  用编辑器 `文件 ▸ 新建 C++ …(系统)` 建的话,**这两行会由编辑器自动写好**。

所以系统里不要存"跨场景/跨运行"的长期状态:Play 停止它就被摘掉了。

## 阶段与顺序

- **阶段**:`PreFixed → Fixed → Update → Late → PreRender`。引擎**每帧按这五个阶段依次跑一遍**,
  写在非 `Update` 阶段的系统真的在那一刻执行。
  - Luau:`ecs:AddSystem(name, fn, { phase = "Late" })` 或 `ecs:AddSystem(name, fn, "Late")`
  - C++:`ISystem::Phase()`
- **同阶段内的顺序**:`after = { "OtherSystem" }`(C++ `ISystem::After()`),表示"我排在它之后"。
  依赖的系统不存在时会**跳过本系统并记一条警告**(不会静默乱序)。

## 复用代码:脚本库

`<内容根>/scripts/lib/*.luau` 放**可复用**的代码,用 `ecs:RequireLib` 装载:

```lua
-- <内容根>/scripts/lib/util/damage.luau
local M = {}
function M.apply(health, amount) health.Value = health.Value - amount end
return M
```

```lua
-- 系统脚本里
local damage = ecs:RequireLib("util/damage")
```

- 名字是**相对 `scripts/lib/` 的逻辑路径**(用 `/` 分隔)。通常**不用写扩展名**
  (`"util/damage"` 会先找 `.luau`、再找 `.lua`);带上 `.luau`/`.lua` 也接受,
  但**其它扩展名会被拒绝**(并给出可读原因);
- **同一个路径只执行一次**,返回值被缓存(文件内容变了会重新执行);
- 库文件跑在**同一份沙箱**里 —— `io` / `os` / `require` / `load` 依旧是禁用的;
- `scripts/lib/` 下的文件**不会**被自动加载(自动加载的只有 `scripts/systems/`)。
