-- Pure ECS Luau System: 玩家移动系统
-- 随场景启动由 ScriptEngine::LoadSystemScripts 自动加载并注册进帧管线

local movementQuery = ecs:Query({"TransformComponent", "VelocityComponent"})

ecs:AddSystem("LuauPlayerMovementSystem", "Update", function(dt)
    movementQuery:Each(function(entity, transform, velocity)
        -- 线速度位移更新
        if velocity.Linear.x ~= 0 or velocity.Linear.y ~= 0 or velocity.Linear.z ~= 0 then
            transform.Location.x = transform.Location.x + velocity.Linear.x * dt
            transform.Location.y = transform.Location.y + velocity.Linear.y * dt
            transform.Location.z = transform.Location.z + velocity.Linear.z * dt
        end
    end)
end)

print("[Pure ECS Luau] LuauPlayerMovementSystem registered into frame pipeline.")
