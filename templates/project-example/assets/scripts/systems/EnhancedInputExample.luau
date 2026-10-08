--!strict
-- =============================================================================
-- WorldEngine 纯 ECS 玩家输入系统 (EnhancedInputSystem.luau)
-- 位于: templates/project-example/assets/scripts/systems/
-- 运行机制 (Pure ECS 架构):
--   1. 场景启动时由 ScriptEngine::LoadSystemScripts 自动加载并执行一次;
--   2. 本脚本调用 `ecs:AddSystem` 将系统注册进场景帧管线的 "Update" 阶段;
--   3. 每帧由管线调度驱动, 用 `Input:Axis` / `Input:Pressed` / `Input:Down` 响应交互,
--      用 `ecs:Query` 批量更新实体组件, 并用 `Input:Rumble` 给予手柄震动反馈。
-- =============================================================================

local inVehicle = false

ecs:AddSystem("PlayerInputSystem", function(dt: number)
    -- 1. 读取水平移动轴输入 (键盘 A/D 或手柄左摇杆 LX)
    local moveAxis = Input:Axis("Move")

    -- 2. 交互状态机响应
    -- (a) Tap: 短促按下 F 键 -> 触发场景物体交互 / 上下车
    if Input:Pressed("Interact") then
        inVehicle = not inVehicle
        if inVehicle then
            print("[PlayerInputSystem] 上车: 切换为载具模式")
            Input:Rumble(0.4, 0.6, 0.25)
        else
            print("[PlayerInputSystem] 下车: 恢复步行模式")
        end
    end

    -- (b) Hold: 长按 Shift -> 持续疾跑
    local isSprinting = Input:Down("Sprint")

    -- (c) Pulse: 按住开火键 -> 全自动连发脉冲
    if Input:Pressed("Fire") then
        print("[PlayerInputSystem] 武器开火: 触发微震动反馈")
        Input:Rumble(0.1, 0.5, 0.08)
    end

    -- (d) DoubleTap: 双击空格 -> 触发战术翻滚闪避
    if Input:Pressed("Dodge") then
        print("[PlayerInputSystem] 触发双击翻滚闪避!")
        Input:Rumble(0.3, 0.3, 0.15)
    end

    -- 3. Pure ECS 查询: 驱动挂载 TransformComponent 的实体
    local query = ecs:Query({ "TransformComponent" })
    query:Each(function(entity: Entity, transform: any)
        if math.abs(moveAxis) > 0.05 then
            local speed = isSprinting and 8.0 or 4.0
            transform.Location.x = transform.Location.x + moveAxis * speed * dt
        end
    end)
end, "Update")

print("[PureECS] PlayerInputSystem 已成功注册进场景 Update 帧管线")
