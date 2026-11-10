# 物理事件与碰撞过滤(Physics ↔ ECS)

> P5(2026-10-03)。面向改引擎的人;做游戏的人看 `docs/user/scripting/` 的 Lua 一节。

## 1. 口径

物理是 ECS 的一等公民:**模拟在固定步长阶段跑,事实以事件的形式交给系统消费。**

- **产出**:`physics-2d` / `physics-3d` 帧系统在 `Fixed` 阶段把后端(Box2D / Jolt)的事件
  翻译成 `World::Physics::ContactEvent` / `TriggerEvent`,推进场景队列。
- **消费**:任意系统在**可变阶段**(`Update` / `Late` / `PreRender`)读
  `Scene::GetContactEvents()` / `GetTriggerEvents()`。
- **清空**:每帧恰好一次 —— 本帧第一个固定步入口清空,并开始累积;
  若本帧一个固定步都没有,则由可变阶段入口清空(读到的永远是"本帧固定步产出的事件")。

⇒ 事件序列**与帧率无关**:同一段真实时间、同一固定步序列输入,事件序列(类型/配对/阶段/顺序)
逐字节可复现。这是 P4(固定步长)的直接延伸,由 `World.PhysicsEventsTests` 的
"frame-rate independent" 用例守着。

## 2. 事件类型

`Engine/src/World/Physics/PhysicsEvents.h`:

| 类型 | 字段 | 说明 |
| --- | --- | --- |
| `ContactEvent` | `EntityA/EntityB`、`Phase`、`Point`、`Normal`、`PenetrationDepth` | 两个实体的**实体接触**。`Normal` 指向 A→B。 |
| `TriggerEvent` | `SensorEntity/OtherEntity`、`Phase` | **传感器**重叠。只报事实,不产生碰撞响应。 |
| `ContactPhase` | `Begin` / `Persist` / `End` | 开始接触 / 持续 / 分离。 |

`End` 阶段几何量一律为零 —— 后端此时已拿不到流形(Box2D 的接触已销毁,Jolt 只给 sub-shape 对)。
`EntityA/EntityB` 的顺序由后端决定(Box2D 按 shape, Jolt 按 body),但在同一固定步输入下确定,
所以事件序列可比对;不要依赖"A 一定是先创建的那个实体"。

## 3. 逐后端映射

| 事实 | Box2D(2D) | Jolt(3D) |
| --- | --- | --- |
| `Begin` | `b2World_GetContactEvents().beginEvents` + `b2Contact_GetData` | `OnContactAdded` |
| `Persist` | 每步 `b2Body_GetContactData` 枚举,排除本步 `Begin` 的配对(去重) | `OnContactPersisted` |
| `End` | `b2World_GetContactEvents().endEvents` | `OnContactRemoved` |
| 传感器 | `b2World_GetSensorEvents()` + `b2Shape_GetContactData` 补 `Persist` | `SetIsSensor` / `mIsSensor`,监听器同样回调 |
| 实体反查 | shape 的 `userData`(`Physics::PackEntityUserData`,实体索引 +1 偏移) | `Body::GetUserData()` |

两个后端**都**提供 `Persist`,所以用户的系统不用为 2D/3D 写两套。
3D 侧 Jolt 的接触回调发生在 `PhysicsSystem::Update()` **内部**:实现先把事件推进缓冲区,
`Step()` 返回后按序 flush —— 顺序确定,也不在回调里碰场景数据结构。

## 4. 碰撞过滤

| 组件 | 字段 | 语义 |
| --- | --- | --- |
| `RigidBody2DComponent` | `Layer` / `Mask`(`uint32` 位掩码) | 该刚体每个 shape 的 `b2Filter.categoryBits` / `maskBits` |
| `RigidBody3DComponent` | `Layer` / `Mask` | Jolt `ObjectLayer` |

判据统一为 **`(MaskA & LayerB) != 0 && (MaskB & LayerA) != 0`**(与 Box2D 一致)。
不满足时:**既不产生接触事件,也不产生碰撞响应**(直接穿透),不是"碰了但不报"。

3D 的落法值得记一笔:Jolt 的 `ObjectLayerPairFilter` 只能看到 `ObjectLayer` 序号,看不到掩码。
所以 `Physics3DWorld` 内部维护一张 **"唯一 `(Layer, Mask)` 组合 ↔ `ObjectLayer` 序号"注册表**,
pair filter 反查该表做上面那条判据。这样 2D/3D 的语义逐条等价,而不是退化成"全局层矩阵"。

> 运行时改 `Layer`/`Mask`(建体之后)目前**不会**自动重建刚体 —— 与既有 2D 组件行为一致,
> 属于后续项(见 plan §17 的 P6/P7 之后的收尾清单)。

## 5. 传感器(Trigger)

| 后端 | 粒度 | 为什么 |
| --- | --- | --- |
| Box2D(2D) | **逐 collider**(`BoxCollider2DComponent.IsSensor` / `CircleCollider2DComponent.IsSensor`) | Box2D 的 `b2ShapeDef.isSensor` 是逐 shape |
| Jolt(3D) | **逐刚体**(`RigidBody3DComponent.IsSensor`) | Jolt 的传感器是 `Body::SetIsSensor`,刚体级 |

**语义差异是真实的,不要假装同构**。想要"3D 里某个形状是触发器",当前等价做法是给该实体单独一个
传感器刚体(不挂其它 collider),或者把整个刚体设成传感器。

## 6. 与旧接口的关系

P5 **删除**了 `Scene::AddPhysics3DContactCallback` / `ClearPhysics3DContactCallbacks` 与
`Physics3DWorld::SetContactCallback(bool, entity, entity)`(裸回调表)。理由:回调表不是 ECS 事件 ——
没有阶段归属、没有实体查询语义、没有 2D 对应物、也不能被脚本面消费。现在统一走场景事件队列。

`Scene::AddComponentObserver` / `OnAdd` / `OnRemove` 仍然存在且**语义不同**:
那一族面向"组件增删",物理事件面向"物理事实",两者并存。
