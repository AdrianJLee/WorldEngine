-- 新建系统脚本模板。
-- 放到 <项目根>/assets/scripts/systems/ 下才会被自动加载 —— 场景启动时整份执行一次,
-- 之后每帧按你注册的阶段调用。
-- 提示来自编辑器生成的 assets/scripts/intermediate/WorldEngineAPI.luau。

-- 支持使用 Comp 常量表避免手写字符串:Comp.Transform 等价于 "TransformComponent"
local query = ecs:Query({ Comp.Transform, Comp.Velocity })

-- 规范签名是 (名字, 函数 [, 阶段]);阶段可直接使用 Phase 枚举(如 Phase.Update)。
-- 也支持单表配置形态: ecs:AddSystem({ name = "NewSystem", update = function(dt: number) ... end, phase = Phase.Update })
ecs:AddSystem("NewSystem", function(dt: number)
    query:Each(function(entity: Entity, transform: any, velocity: any)
        transform.Location.x = transform.Location.x + velocity.Linear.x * dt
        transform.Location.y = transform.Location.y + velocity.Linear.y * dt
        transform.Location.z = transform.Location.z + velocity.Linear.z * dt
    end)
end, Phase.Update)

-- 需要"某个组件出现/消失时做一次事",用观察者代替轮询:
-- ecs:OnAdd(Comp.Tag, function(entity: Entity) print("tag added", entity:GetName()) end)
