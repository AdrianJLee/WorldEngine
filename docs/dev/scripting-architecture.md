# 脚本与系统架构（纯 ECS）

> 本文是"数据、逻辑、脚本"三者边界的**唯一**开发者口径。改动脚本层行为时同批更新本文件。
> 用户视角见 `docs/user/scripting/`。

## 1. 三层划分

引擎里"脚本"不是挂在实体上的东西,而是**世界（Scene）上的东西**。三层各司其职:

| 层 | 是什么 | 写在哪 | 谁在用 |
| --- | --- | --- | --- |
| **组件** | 纯数据（POD）+ 标签 | C++ 头文件,`WE_SCHEMA_BODY` 反射 | 实体通过 `AddComponent` 装配;属性面板/存档/预制体走这一份 |
| **系统** | 逻辑 | C++ `World::ISystem`,或 Luau `scripts/systems/*.luau` | 场景帧管线（`SystemRegistry`） |
| **脚本库** | 纯函数/配置表 | `<内容根>/scripts/lib/*.luau` | 系统脚本用 `ecs:RequireLib("util/math")` 装载(同路径只执行一次,内容变了重跑);**不会自动加载** |

**硬规则:Luau 不能定义组件类型。** 组件类型只由 C++ 的 schema 反射定义。

理由:组件的类型与字段要同时被三个时机看见 —— **编辑期**（属性面板列字段）、**读档期**
（`.wd` 按类型名写字段）、**运行期**（Lua 按字段读写）。C++ 组件在编译期就固定了 schema,
三个时机都免费;运行期新增类型则要在"编辑器不跑游戏逻辑"的前提下也拿到 schema,那是一条
独立子系统（动态存储 + schema 分发 + 编辑器预加载 + 预制体覆盖 + 字段迁移）。

**Lua 侧的类型安全由共享库兜底**:需要 Lua 视野里的"结构"时,在 `scripts/lib/` 里声明
`---@class` 注解表,系统脚本引用它拿补全 —— 它只是注解,不参与存储与序列化。

**边界(2026-10-02,PECS-T13 起)**:Lua 的 `require`/`load`/`io`/`os`/`package`/`debug` 等全局
**故意**保持置空(`Engine/src/World/Script/Vm/LuauVm.cpp` 的 `kForbiddenGlobals`);库文件的
装载走**专用通道** `ecs:RequireLib(name)` —— 名字是相对 `scripts/lib/` 的逻辑路径
(`BuildScriptLibCandidates`:无扩展名 ⇒ 依次试 `.luau`/`.lua`;已带 `.luau`/`.lua` ⇒ 视作完整
相对路径;带**其它**扩展名 ⇒ 可读拒绝),
规范化的逃逸防护复用 `Vfs::Normalize`(同 `ResolveScriptDiskPath`),执行前做
"落在 `scripts/lib/` 之下"的前缀比较;按路径缓存(内容哈希变化重跑)、循环依赖报错、
库文件跑在**同一份沙箱全局**下。只有 `<内容根>/scripts/systems/*.luau` 会在场景启动时自动执行。

## 2. 组件:纯数据

- 一个组件 = 一个 `WE_SCHEMA_BODY(World|Game, Name, Component)` 的 C++ struct,只放字段。
- 引擎不提供任何"带逻辑的组件"。旧的两条路（`ScriptableEntity` 虚函数面、
  `CppScriptComponent` / `LuauScriptComponent` 单实体脚本）**已整体删除**。
- 运行时状态（句柄、缓存）**不放进组件**:组件保持平凡可拷贝,句柄住在系统/场景的内部表里
  —— 参考 2D 物理的 `Scene::m_PhysicsBodies2D` / `m_PhysicsJoints2D`（与 3D 的
  `Physics3DWorld::Impl::m_Bodies` 同构）。纯缓存若要复用 ECS 存储,用非 schema 组件
  （如 `WorldTransformComponent` / `PhysicsInterpolationState`）。
- 标签组件就是零字节 struct（`struct DeadTag {};`）,给查询当过滤条件。

## 3. 系统:C++ 与 Luau 平权

两条路进的是**同一个** `Gameplay::SystemRegistry`（阶段 + 同阶段 `After` 依赖 + 并行标记）:

**C++** —— 派生 `World::ISystem`,在场景启动时挂上:

```cpp
class MovementSystem : public World::ISystem
{
public:
    std::string_view Name() const override { return "MovementSystem"; }
    Gameplay::SystemPhase Phase() const override { return Gameplay::SystemPhase::Update; }
    bool ParallelSafe() const override { return true; }
    void Update(Scene& scene, Timestep dt) override;   // 内部用 scene.Query<...>().Each(...)
};
```

**项目侧怎么挂上去**:模块加载时还没有场景,场景创建时模块拿不到通知,所以引擎给了一个
显式的挂载时机 —— `WorldContext` 的场景钩子(`Engine/src/World/Core/WorldContext.h`):

```cpp
// <项目根>/src/GameProject.cpp   (声明在 Game/src/GameAPI.h)
namespace World::Game
{
    void AttachProjectSystems(Scene& scene)
    {
        scene.RegisterSystem<MovementSystem>();
    }

    void DetachProjectSystems(Scene& scene)
    {
        scene.UnregisterFrameSystem("MovementSystem");   // 名字必须与 Name() 逐字相同
    }
}
```

Game 模块在 `Register(context)` 里把这一对回调登记进 `WorldContext`;引擎在
`Scene::OnRuntimeStart()` 末尾调用 `Attach`、`Scene::OnRuntimeStop()` 开头调用 `Detach`。
因此**系统只活在一次运行时内**:Play 停止 → 系统撤销,再次 Play → 重新挂上(与 Lua 系统脚本
同一生命周期)。`src/GameProject.cpp` 是**可选**文件 —— 项目没有它时编一份空实现,
构建不会因为少一个文件而失败(构建期探测,见 `Game/CMakeLists.txt`)。

引擎自身的系统(物理 / movement / transform / camera)不走这条路:它们由
`Scene::EnsureDefaultFrameSystems()` 在同一个注册表里挂好,`Attach` 只会**追加**在它们之后。

**Luau** —— 项目内容树下的系统脚本,场景启动时整份执行一次:

```lua
-- <内容根>/scripts/systems/player_movement.luau
local q = ecs:Query({ "TransformComponent", "VelocityComponent" })

ecs:AddSystem("LuauPlayerMovementSystem", function(dt)
    q:Each(function(entity, transform, velocity)
        transform.Location.x = transform.Location.x + velocity.Linear.x * dt
    end)
end, "Update")
```

加载时机:`Scene::OnRuntimeStart()` → `ScriptEngine::LoadSystemScripts(scene)`。编辑器 Play、
编辑器 Simulate、独立 Runtime 走同一条路径。

**阶段与顺序依赖真的生效**:`Scene::RunFrameSystems` 每帧按
`PreFixed → Fixed → Update → Late → PreRender` **依次跑满五个阶段**,同阶段内按
`After` 做拓扑排序 —— 所以 `ISystem::Phase()` / `After()` 与 Lua 的
`{ phase = …, after = { … } }` 都能决定执行时机(2026-10-02 之前这两条在 Scene 宿主路径上
没有接线:注册被硬编码成 `Update`、派发也只跑 `Update`,非 `Update` 阶段的系统**永不执行**)。

**引擎内置的帧系统(2026-10-02 起共 7 个)**:`physics-2d` / `physics-3d` /
`movement-system`(以上 **`Fixed`** —— 固定步长)、`transform-system` / `camera-system`(`Update`)、
`animation-system` / `render-extract`(`PreRender`,抽取声明 `After(animation-system)`)。

**固定步长(工业口径)**:物理(Box2D / Jolt 两侧)与移动跑 `Fixed` 阶段,**dt 恒为
`1/FixedStepHz`** —— 由 `GameApp` 的累加器决定"这一帧跑 0..N 步",宿主(`GameHost`)只负责
把固定回调接到 `Scene::OnFixedUpdate`。⇒ **同一段真实时间,不管帧率多少,模拟结果一致**
(实测:60fps 与 30fps 逐位相同;改成 `Update` 阶段的可变 dt 则不一致)。
表现层(transform/camera/animation/抽取)留在可变阶段,看到的是本帧**最后一次**物理步之后的位姿
(顺序:`Fixed(0..N) → Update → Late → PreRender`)。
后两个此前埋在 `SceneRenderer` 里、不在管线中;现在它们是**每帧一次且幂等**的帧内步骤
(`Scene::EnsureWorldTransforms` / `EnsureAnimationAdvanced` / `EnsureRenderExtract`)——
Play/Simulate 走帧系统,**编辑态不跑帧系统**,由渲染前兜底跑同一份实现
(所以编辑器里拖父项子项仍跟随、蒙皮预览仍动)。
渲染抽取的结果落在**场景**上(`Scene::RenderExtract()`):收集是**相机无关**的(所以能进管线),
而视锥剔除、阴影矩阵与各 pass 依赖相机,留在提交侧(相机由宿主在提交时给)。

### `ecs` 全局表（也是 `world`）

| 方法 | 作用 |
| --- | --- |
| `ecs:Query({ "CompA", "CompB" })` | 建查询;结果有 `:Each(fn)` 与 `:Count()` |
| `ecs:Query({ "CompA" }, { without = { "DeadTag" } })` | 排除过滤(对应 C++ 的 `Query::Without<...>`);未知键/未注册名报可读错误 |
| `ecs:AddSystem(name, fn [, phase])` | 注册具名系统;**幂等**（同名先撤销再注册） |
| `ecs:AddSystem(name, fn, { phase = "Late", after = { "Other" } })` | 阶段 + 同阶段顺序依赖(落到 `SystemDesc::After`) |
| `ecs:RemoveSystem(name)` | 撤销具名系统 |
| `ecs:CreateEntity([name])` | 建实体（Tag + UUID） |
| `ecs:DestroyEntity(entity)` | 排队销毁（安全点提交） |
| `ecs:EntityCount()` | 存活实体数 |
| `ecs:OnAdd(comp, fn)` / `ecs:OnRemove(comp, fn)` / `ecs:Off(handle)` | 响应式组件观察者 |
| `ecs:OnContact(fn)` / `ecs:OnTrigger(fn)` | 本帧接触 / 传感器事件(载荷为表,见下);返回句柄,同样交给 `ecs:Off` |

表的**唯一描述源**在 `Engine/src/World/Script/Bindings/BindECS.h` 的 `ScriptEcsBindings()`;
运行时注册循环与 Lua 存根渲染共用它,两侧不会漂移。

**物理事件(Lua 面)**:`Scene::GetContactEvents()` / `GetTriggerEvents()` 是"固定步产出、
每帧恰好清空一次"的队列(`RunFixedFrameSystems` / `RunFrameSystems`)。绑定层在脚本侧注册
帧系统 `lua-physics-events`(**Late** 阶段)做每帧派发 —— 不给 `Scene.cpp` 加钩子
(与 `ecs:AddSystem` 同一条 `Scene::RegisterFrameSystem` 通道)。回调载荷:接触
`{ a, b, phase, point = {x,y,z}, normal = {x,y,z}, depth }`,触发 `{ sensor, other, phase }`;
实体用与 `ecs` 面一致的 **Entity userdata**。同一帧按事件在队列里的顺序派发(确定性),
单个回调报错只记日志、不打断其它订阅者。**2D 与 3D 的语义差异**:2D 的传感器是**逐 shape**
(`BoxCollider2D` / `CircleCollider2D`),3D 的传感器是**逐刚体**(Jolt 原生模型);
`Begin` / `Persist` / `End` 两侧都提供。句柄生命周期与组件观察者一致:只有 `ecs:Off` 显式
注销,系统脚本热重载是整份重跑(再次订阅即多一条,与 `OnAdd` 同口径)。

### 需要"每个实体一段逻辑"时

单实体脚本已经没有了。对应写法:

| 旧写法 | 纯 ECS 写法 |
| --- | --- |
| 实体挂脚本、`OnCreate` | 系统脚本里 `ecs:OnAdd("SomeComponent", fn)` |
| `OnUpdate` | `ecs:AddSystem` 里的 `Query:Each` |
| `OnDestroy` | `ecs:OnRemove(...)`,或系统里扫"待销毁"标记 |
| `OnUI` | 见 §6（当前未恢复） |

## 4. 系统脚本热重载

- `ScriptEngine::PollSystemScriptReload(scene, dt)` 每帧驱动（`Scene::OnUpdateRuntime` 里调用）,
  轮询 `<内容根>/scripts/systems/`;**内容哈希优先**,改回原值不算变化,150ms 消抖。
- 单份脚本重载 = **整份重跑**:先按归属表撤销该文件上次注册的系统,再重读源、整份执行。
  归属表由 `ecs:AddSystem` 在"正在执行的系统脚本"作用域内自动登记。
- 场景停止时 `UnloadSystemScripts` 撤销全部系统脚本注册的系统。

**边界**:宿主手写 `ecs:AddSystem`（不在系统脚本加载作用域内）的系统**不参与**脚本热重载 ——
它们的生命周期由宿主自己管。

## 5. 存根（LSP 补全）

- 编辑器按实际注册的绑定生成 `<内容根>/assets/scripts/intermediate/WorldEngineAPI.luau`;
  组件块来自 `SchemaRegistry`,服务/事件/UI/插件/`ecs` 块来自各自的描述表。
- **不要** `require` 或运行它;它是声明基线。
- 漂移门禁:`World.ScriptWorkflow` 逐字节比较"渲染结果"与入库文件。

## 6. 待定：脚本 UI（`ui.*`）

旧的 `OnUI` 生命周期随单实体脚本一起删除。`ScriptEngine::DrawScriptUi` 目前是返回 0 的兼容占位,
脚本里调 `ui.*` **不会报错但也没有效果**。

恢复方向要在"系统模式下 UI 怎么表达"这一层定（是 PreRender 阶段的 UI 系统,还是声明式 UI 数据 + 一个渲染系统）,
属于单独一次架构讨论,不在本轮范围内。在那之前,不要依赖 `ui.*`。

## 7. 历史（已删除，不要回退）

- `ScriptableEntity` 虚函数基类 —— 删除于 M7。
- `CppScriptComponent` / `LuauScriptComponent` 单实体脚本组件、`BehaviorRegistry` 的运行时描述、
  单实体脚本属性进场景（`.wd` 的 `Scripts:` 块）—— 删除于 M8。
- 判别依据:这三样都把"逻辑"绑在实体上,与纯 ECS 的数据/逻辑分离直接冲突。
