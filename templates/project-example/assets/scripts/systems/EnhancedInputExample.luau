-- =============================================================================
-- WorldEngine 纯 ECS 玩家输入系统 (EnhancedInputExample.luau)
-- 位于: templates/project-example/assets/scripts/systems/
-- 运行机制:
--   1. 场景启动时由 ScriptEngine::LoadSystemScripts 自动加载并注册进帧管线;
--   2. 采用 Pure ECS 架构, 使用 ecs:AddSystem 注册进 "Update" 阶段;
--   3. 响应输入动作, 并在交互时**逻辑主动打开/关闭游戏 UI (不自动附加)**。
--
-- 输入语义(R1 起触发器真正参与解算 —— 见 assets/input.weinput 的 triggers):
--   Input.Pressed(name)  = 该动作的触发器**本帧触发**。五类交互因此各自只在该响的时候响:
--                          裸绑定=按下那一帧;hold=按住达到时长那一帧;tap=限时内抬起那一帧;
--                          double_tap=第二击;pulse=按住期间每隔 interval 一次。
--   Input.Down(name)     = 动作当前处于激活态(含触发前的进行中,例如按住但还没到 hold 阈值)。
--   Input.Released(name) = 激活态的下降沿。
--   注意:触发器写在 contexts 的 mapping 上 —— 只有该上下文**处于激活态**时才生效。
--   Walking 在映射表里标了 default: true(装载即自动压栈);Vehicle 由本脚本按需压栈/出栈。
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
        -- 上下文切换:F 键在载具里是"下车",语义不同 —— 靠上下文覆盖,而不是在脚本里堆 if。
        if inVehicle then
            Input.PushContext("Vehicle")
            print("[PlayerInputSystem] 已压入输入上下文 Vehicle(空格改为手刹)")
            Input.Rumble(0.4, 0.6, 0.25)
        else
            Input.PopContext("Vehicle")
            print("[PlayerInputSystem] 已弹出输入上下文 Vehicle(恢复步行语义)")
        end
    end

    -- (b) Hold: 长按 Shift -> 持续疾跑。
    --     Down  = 按住即真;想要"必须按住满 0.3s 才起效"用 Input.Pressed("Sprint")
    --     (hold 触发器只在跨过阈值那一帧触发一次)。
    local isSprinting = Input.Down("Sprint")

    -- (c) Pulse: 按住开火键 -> 全自动脉冲连发(pulse 触发器每个 interval 触发一次)
    if Input.Pressed("Fire") then
        print("[PlayerInputSystem] 武器开火: 触发微震动")
        Input.Rumble(0.1, 0.5, 0.08)
    end

    -- (d) DoubleTap: 双击空格 -> 战术翻滚闪避(单击不会触发)
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
