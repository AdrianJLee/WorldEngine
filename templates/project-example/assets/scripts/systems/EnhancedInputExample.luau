-- =============================================================================
-- WorldEngine 纯 ECS 玩家输入系统 (EnhancedInputExample.luau)
-- 位于: templates/project-example/assets/scripts/systems/
-- 运行机制:
--   1. 场景启动时由 ScriptEngine::LoadSystemScripts 自动加载并注册进帧管线;
--   2. 采用 Pure ECS 架构, 使用 ecs:AddSystem 注册进 "Update" 阶段;
--   3. 响应输入动作, 并在交互时**逻辑主动打开/关闭游戏 UI (不自动附加)**。
-- =============================================================================

local inVehicle = false

ecs:AddSystem("PlayerInputSystem", "Update", function(dt: number)
    -- 1. 读取移动轴输入 (键盘 A/D 或手柄左摇杆 LX)
    local moveAxis = Input.Axis("Move")

    -- 2. 交互状态机响应
    -- (a) Tap: 短按 F 键 -> 逻辑主动打开 / 关闭 HUD UI (纯逻辑驱动，无静默自动附加)
    if Input.Pressed("Interact") then
        if ui.isOpen() then
            print("[PlayerInputSystem] 玩家主动关闭 HUD UI")
            ui.close()
        else
            print("[PlayerInputSystem] 玩家主动打开 HUD UI: 'assets/ui/hud.wui'")
            ui.open("assets/ui/hud.wui")
        end

        inVehicle = not inVehicle
        if inVehicle then
            Input.Rumble(0.4, 0.6, 0.25)
        end
    end

    -- (b) Hold: 长按 Shift -> 持续疾跑
    local isSprinting = Input.Down("Sprint")

    -- (c) Pulse: 按住开火键 -> 全自动脉冲连发
    if Input.Pressed("Fire") then
        print("[PlayerInputSystem] 武器开火: 触发微震动")
        Input.Rumble(0.1, 0.5, 0.08)
    end

    -- (d) DoubleTap: 双击空格 -> 战术翻滚闪避
    if Input.Pressed("Dodge") then
        print("[PlayerInputSystem] 触发双击翻滚闪避!")
        Input.Rumble(0.3, 0.3, 0.15)
    end

    -- 3. 纯 ECS 查询: 驱动挂载 TransformComponent 的实体移动
    local query = ecs:Query({ "TransformComponent" })
    query:Each(function(e, t)
        if math.abs(moveAxis) > 0.05 then
            local speed = isSprinting and 8.0 or 4.0
            t.Location = t.Location + vec3.new(moveAxis * speed * dt, 0.0, 0.0)
        end
    end)
end)

print("[PureECS] PlayerInputSystem 已成功注册进场景 Update 帧管线")
