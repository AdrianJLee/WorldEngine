-- 新建系统脚本模板。
-- 放到 <项目根>/assets/scripts/systems/ 下才会被自动加载 —— 场景启动时整份执行一次,
-- 之后每帧按你注册的阶段调用。
-- 提示来自编辑器生成的 assets/scripts/intermediate/WorldEngineAPI.luau。

local query = ecs:Query({ "TransformComponent", "VelocityComponent" })

ecs:AddSystem("NewSystem", "Update", function(dt)
    query:Each(function(entity, transform, velocity)
        transform.Location.x = transform.Location.x + velocity.Linear.x * dt
        transform.Location.y = transform.Location.y + velocity.Linear.y * dt
        transform.Location.z = transform.Location.z + velocity.Linear.z * dt
    end)
end)

-- 需要"某个组件出现/消失时做一次事",用观察者代替轮询:
-- ecs:OnAdd("DeadTag", function(entity) print("died", entity:GetName()) end)
