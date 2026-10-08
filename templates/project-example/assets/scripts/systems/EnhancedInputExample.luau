--!strict
-- =============================================================================
-- WorldEngine 增强输入系统玩法控制器示例 (EnhancedInputExample.luau)
-- 位于示例工程模板: templates/project-example/assets/scripts/systems/
-- 演示能力:
--   1. 上下文栈 (Walking ↔ Vehicle) 动态压栈与出栈
--   2. 交互状态机响应: Hold 奔跑, Tap 互动, DoubleTap 闪避翻滚, Pulse 连发
--   3. 修改器应用: 死区消除、摇杆曲线精准瞄准、WASD 维度重组
--   4. 手柄震动反馈 (Rumble) 动作级闭环
-- =============================================================================

local ecs = require("@world/ecs")
local input = require("@world/services/input")
local time = require("@world/services/time")

local ControllerState = {
    InVehicle = false,
    SprintSpeed = 8.0,
    WalkSpeed = 4.0,
    AimSensitivity = 1.5,
}

-- 初始化输入系统与上下文
function OnCreate(self: any)
    print("[EnhancedInput] 初始化玩家输入控制器示例")
    
    -- 默认激活基础步行映射上下文
    if input.PushContext then
        input.PushContext("Walking", 0)
    end
end

-- 帧系统更新回调
function OnUpdate(self: any, dt: number)
    -- 1. 读取水平移动轴输入 (键盘 A/D 或手柄左摇杆 LX)
    local moveAxis = input.Axis("Move")

    -- 2. 检查交互状态机
    -- (a) Tap: 短促按下 F 键 -> 触发场景物体交互 / 上下车
    if input.Pressed("Interact") then
        ControllerState.InVehicle = not ControllerState.InVehicle
        if ControllerState.InVehicle then
            print("[EnhancedInput] 上车: 压入 Vehicle 上下文 (优先级 10 覆盖步行键位)")
            if input.PushContext then
                input.PushContext("Vehicle", 10)
            end
            -- 引擎点火震动反馈: 左马达 0.4, 右马达 0.6, 持续 0.25 秒
            input.Rumble(0.4, 0.6, 0.25)
        else
            print("[EnhancedInput] 下车: 弹出 Vehicle 上下文 (恢复步行)")
            if input.PopContext then
                input.PopContext("Vehicle")
            end
        end
    end

    -- (b) Hold: 长按 Shift -> 触发持续疾跑
    local isSprinting = input.Down("Sprint")
    local currentSpeed = isSprinting and ControllerState.SprintSpeed or ControllerState.WalkSpeed

    -- (c) Pulse: 按住鼠标左键开火 -> 全自动连发脉冲
    if input.Pressed("Fire") then
        print("[EnhancedInput] 武器开火: 触发微震动反馈")
        input.Rumble(0.1, 0.5, 0.08)
    end

    -- (d) DoubleTap: 双击空格 -> 触发战术翻滚闪避
    if input.Pressed("Dodge") then
        print("[EnhancedInput] 触发双击翻滚闪避!")
        input.Rumble(0.3, 0.3, 0.15)
    end
end

-- 卸载清理
function OnDestroy(self: any)
    if input.PopContext then
        input.PopContext("Vehicle")
        input.PopContext("Walking")
    end
end
