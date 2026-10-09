-- =============================================================================
-- WorldEngine 输入闭环示例 (EnhancedInputExample.luau)
-- =============================================================================
-- 这份脚本与 **assets/input.weinput** 是一对演示,两者必须逐项对得上:
--   * 映射里声明的每个 action / axis / context,本脚本都消费(否则那个声明在演示里是死的);
--   * 本脚本用到的每个名字,都必须在映射里真实存在(否则脚本读到的恒为 false)。
--   这条一致性由门禁 `verify-example-input-loop.py` 双向校验(静态对齐 + 实机跑通)。
--
-- 一行一事的对照表(左 = 映射里的一行,右 = 本脚本打印什么):
--   action Jump     空间(空格)      Walking -> "Jump Walking=跳跃";Vehicle -> "Jump Vehicle=手刹"
--                                   ↑ 同一个动作、同一个按键,靠 contexts 覆盖语义 —— 这就是上下文的演示点
--   action Sprint  LeftShift        hold 0.3s 跨过阈值那一帧 -> "Sprint (hold)";松开 -> 结束
--   action Interact  F              tap 0.25s -> 上车/下车(压栈/弹栈 Vehicle)+ 开关 HUD
--   action Dodge    Space 双击      double_tap -> "Dodge (double_tap)"(单击不会触发)
--   action Fire     LMB             pulse 0.12s -> 按住期间每个脉冲打一行 + 微震动
--   axis   Move     A/D 或 LX       方向变化时打一行(而不是每帧刷屏)
--   context Walking default: true   装载即自动压栈(引擎侧行为,无需脚本)
--   context Vehicle 由 Interact(F) 压栈/弹栈
--
-- 与 GameUiStartupSystem.luau 的分工(两个演示不打架):
--   * 启动系统负责"场景开始时**弹出**HUD 一次"(演示原生生命周期 API);
--   * 本脚本负责"玩家按 F 可以关掉/重新打开它"(演示输入 -> UI 的闭环)。
--   两份脚本操作的是同一个 HUD 文档,方向不同,不重复。
-- =============================================================================

-- 只在**方向变化**时打印轴(每帧打印会把日志刷没)
local lastMoveSign = 0
local registered = false

local function demo(tag: string, detail: string?)
    if detail then
        print(string.format("[INPUT-DEMO] %-22s %s", tag, detail))
    else
        print(string.format("[INPUT-DEMO] %s", tag))
    end
end

ecs:AddSystem("PlayerInputSystem", "Update", function(dt: number)
    if not registered then
        registered = true
        demo("consume assets/input.weinput", "(7 actions / 1 axis / 2 contexts)")
    end

    -- 1) 轴:Move(A/D 或手柄 LX)。
    local moveAxis = Input.Axis("Move")
    local sign = 0
    if moveAxis > 0.05 then
        sign = 1
    elseif moveAxis < -0.05 then
        sign = -1
    end
    if sign ~= lastMoveSign then
        if sign == 0 then
            demo("Move axis", string.format("%+.2f -> idle", moveAxis))
        else
            demo("Move axis", string.format("%+.2f -> %s", moveAxis,
                sign > 0 and "MoveRight(正向)" or "MoveLeft(反向)"))
        end
        lastMoveSign = sign
    end

    -- 2) Jump:同一个动作、同一个空格键,按上下文给出**不同语义**。
    --    映射侧:Walking 的 Jump 是跳跃,Vehicle 的 Jump 是手刹(见 input.weinput 的注释)。
    if Input.Pressed("Jump") then
        if Input.HasContext("Vehicle") then
            demo("Jump", "Vehicle 上下文:空格 = 手刹(contexts 覆盖了同一个动作)")
        else
            demo("Jump", "Walking 上下文:空格 = 跳跃")
        end
    end

    -- 3) Dodge:双击空格才会触发(double_tap 触发器;单击留给上面的 Jump)。
    if Input.Pressed("Dodge") then
        demo("Dodge (double_tap)", "双击空格 -> 战术翻滚")
        Input.Rumble(0.3, 0.3, 0.15)
    end

    -- 4) Sprint:hold 触发器只在跨过 0.3s 阈值那一帧触发一次;持续奔跑看 Down。
    local isSprinting = Input.Down("Sprint")
    if Input.Pressed("Sprint") then
        demo("Sprint (hold)", "按住达到 0.3s 阈值 -> 疾跑开始")
    end
    if Input.Released("Sprint") then
        demo("Sprint (hold)", "松开 -> 疾跑结束")
    end

    -- 5) Fire:pulse 触发器 —— 按住期间每隔 interval(0.12s)响一次。
    if Input.Pressed("Fire") then
        demo("Fire (pulse)", "脉冲连发(interval 0.12s)")
        Input.Rumble(0.1, 0.5, 0.08)
    end

    -- 6) Interact:tap 触发器(短按)。它同时是"上下文的状态开关"和"输入 -> UI 的闭环":
    --    步行时按 F = 上车(压入 Vehicle);载具里按 F = 下车(弹出 Vehicle)。
    if Input.Pressed("Interact") then
        if Input.HasContext("Vehicle") then
            Input.PopContext("Vehicle")
            demo("Interact (tap)", "下车 -> 弹出上下文 Vehicle(恢复步行语义)")
        else
            Input.PushContext("Vehicle")
            demo("Interact (tap)", "上车 -> 压入上下文 Vehicle(空格改为手刹)")
            Input.Rumble(0.4, 0.6, 0.25)
        end
        -- 输入 -> UI 闭环:关掉/重新打开启动系统弹出的那份 HUD(同一份文档,不抢所有权)。
        if ui.isOpen() then
            ui.close()
            demo("UI", "关闭 HUD('assets/ui/hud.wui')")
        else
            if ui.open("assets/ui/hud.wui") then
                demo("UI", "打开 HUD('assets/ui/hud.wui')")
            else
                demo("UI", "打开 HUD 失败(检查文档路径)")
            end
        end
    end

    -- 7) 纯 ECS 查询:轴 + 疾跑状态驱动挂 TransformComponent 的实体。
    local query = ecs:Query({ "TransformComponent" })
    query:Each(function(e, t)
        if math.abs(moveAxis) > 0.05 then
            local speed = isSprinting and 8.0 or 4.0
            t.Location = t.Location + vec3.new(moveAxis * speed * dt, 0.0, 0.0)
        end
    end)
end)

print("[INPUT-DEMO] PlayerInputSystem 已注册(消费 assets/input.weinput 的 7 个动作 / 1 根轴 / 2 个上下文)")
