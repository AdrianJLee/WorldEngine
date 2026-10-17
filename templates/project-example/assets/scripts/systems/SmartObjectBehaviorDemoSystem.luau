-- =============================================================================
-- M3 智能交互对象 (Smart Objects) 与通用行为树 (BT) & 有限状态机 (FSM) 演示系统
-- =============================================================================
-- 本系统演示 WorldEngine 工业级通用 Gameplay AI 架构:
--   1. Smart Objects (智能交互对象):
--      - 空间插槽对齐 (SlotOffset, FacingAngleDeg, InteractionDistance)
--      - 状态机仲裁 (Available -> Claimed -> InUse -> Cooldown)
--      - 运行时插槽世界坐标计算与抢占保护
--   2. Universal Behavior Tree (通用行为树):
--      - 复合控制节点: SelectorNode (高优选择), SequenceNode (顺序推进)
--      - 装饰/条件节点: ConditionNode (动态谓词求值)
--      - 执行/动作节点: ActionNode (行为逻辑执行)
--   3. Finite State Machine (有限状态机 / FSM):
--      - 状态生命周期: Enter / Update / Exit
--      - 状态枚举: Patrolling -> SeekingSmartObject -> Interacting -> CoolingDown
-- =============================================================================

-- -----------------------------------------------------------------------------
-- 1. 通用行为树 (Behavior Tree) 运行时轻量级 Luau 实现
-- -----------------------------------------------------------------------------
local NodeStatus = {
    Success = 0,
    Failure = 1,
    Running = 2,
}

local function CreateCondition(predicate)
    return {
        Type = "Condition",
        Tick = function(self, dt)
            if predicate() then
                return NodeStatus.Success
            end
            return NodeStatus.Failure
        end,
        Reset = function(self) end,
    }
end

local function CreateAction(actionFn)
    return {
        Type = "Action",
        Tick = function(self, dt)
            return actionFn(dt)
        end,
        Reset = function(self) end,
    }
end

local function CreateSequence(children)
    local currentIndex = 1
    return {
        Type = "Sequence",
        Children = children,
        Tick = function(self, dt)
            while currentIndex <= #self.Children do
                local status = self.Children[currentIndex]:Tick(dt)
                if status == NodeStatus.Running then
                    return NodeStatus.Running
                elseif status == NodeStatus.Failure then
                    currentIndex = 1
                    return NodeStatus.Failure
                end
                currentIndex = currentIndex + 1
            end
            currentIndex = 1
            return NodeStatus.Success
        end,
        Reset = function(self)
            currentIndex = 1
            for _, child in ipairs(self.Children) do
                child:Reset()
            end
        end,
    }
end

local function CreateSelector(children)
    local currentIndex = 1
    return {
        Type = "Selector",
        Children = children,
        Tick = function(self, dt)
            while currentIndex <= #self.Children do
                local status = self.Children[currentIndex]:Tick(dt)
                if status == NodeStatus.Running then
                    return NodeStatus.Running
                elseif status == NodeStatus.Success then
                    currentIndex = 1
                    return NodeStatus.Success
                end
                currentIndex = currentIndex + 1
            end
            currentIndex = 1
            return NodeStatus.Failure
        end,
        Reset = function(self)
            currentIndex = 1
            for _, child in ipairs(self.Children) do
                child:Reset()
            end
        end,
    }
end

-- -----------------------------------------------------------------------------
-- 2. 系统主状态与数据定义
-- -----------------------------------------------------------------------------
local initialized = false
local aiAgentEntity = nil
local aiAgentId = nil

local healthStationEntity = nil
local healthStationId = nil

local restBenchEntity = nil
local restBenchId = nil

-- FSM 状态机
local FsmState = {
    Patrolling = "Patrolling",
    SeekingSmartObject = "SeekingSmartObject",
    Interacting = "Interacting",
    CoolingDown = "CoolingDown",
}

local currentFsmState = FsmState.Patrolling
local interactTimer = 0.0
local interactDuration = 2.0 -- 智能对象使用耗时
local targetSmartObjectId = nil
local targetSlotPos = { x = 0, y = 0, z = 0 }

-- 巡逻参数
local patrolDir = 1.0
local patrolSpeed = 2.0
local patrolLeftLimit = -3.0
local patrolRightLimit = 3.0

-- 诊断与日志定时器
local logTimer = 0.0
local lastHKey = false
local lastRKey = false

-- 行为树根节点
local behaviorTreeRoot = nil

-- -----------------------------------------------------------------------------
-- 3. 行为树构建
-- -----------------------------------------------------------------------------
local function BuildBehaviorTree()
    -- 分支 A: 紧急低血量医疗充能 (高优先级)
    local emergencyHealSequence = CreateSequence({
        -- 节点 1: 条件判定 - 血量是否低于 60 点
        CreateCondition(function()
            if not aiAgentId then return false end
            local info = Health.Get(aiAgentId)
            return (info ~= nil and info.CurrentHealth < 60.0)
        end),

        -- 节点 2: 动作 - 检索并预约 (Claim) 医疗智能对象
        CreateAction(function(dt)
            if currentFsmState == FsmState.SeekingSmartObject or currentFsmState == FsmState.Interacting then
                return NodeStatus.Success
            end

            if not healthStationId or not aiAgentId then
                return NodeStatus.Failure
            end

            -- 尝试预约锁定插槽 (防止其他 NPC 争抢)
            local claimed = SmartObject.Claim(healthStationId, aiAgentId)
            if claimed or SmartObject.GetState(healthStationId) == 1 then -- 1 = Claimed
                targetSmartObjectId = healthStationId
                targetSlotPos = SmartObject.GetPosition(healthStationId)
                currentFsmState = FsmState.SeekingSmartObject
                print(string.format("[AI-BT] >> 决策命中: 低血量紧急避险! 成功预约医疗站 [ID: %d], 槽位目标=(%.2f, %.2f, %.2f)",
                    healthStationId, targetSlotPos.x, targetSlotPos.y, targetSlotPos.z))
                return NodeStatus.Success
            end
            return NodeStatus.Failure
        end),

        -- 节点 3: 动作 - 移动接近并使用 (Use) 智能对象
        CreateAction(function(dt)
            if not targetSmartObjectId or not aiAgentId or not aiAgentEntity then
                return NodeStatus.Failure
            end

            local tr = aiAgentEntity:GetComponent("TransformComponent")
            if not tr then return NodeStatus.Failure end

            -- 测算当前位置与插槽目标的距离
            local dx = targetSlotPos.x - tr.Location.x
            local dz = targetSlotPos.z - tr.Location.z
            local dist = math.sqrt(dx * dx + dz * dz)

            -- 若未到达插槽，平滑移动寻路对齐
            if dist > 0.4 then
                currentFsmState = FsmState.SeekingSmartObject
                local moveStep = patrolSpeed * 1.5 * dt
                local loc = tr.Location
                loc.x = loc.x + (dx / dist) * moveStep
                loc.z = loc.z + (dz / dist) * moveStep
                tr.Location = loc
                return NodeStatus.Running
            end

            -- 已到达插槽，开始使用智能对象
            if currentFsmState ~= FsmState.Interacting then
                local used = SmartObject.Use(targetSmartObjectId, aiAgentId)
                if used then
                    currentFsmState = FsmState.Interacting
                    interactTimer = 0.0
                    print(string.format("[AI-BT] >> 到达插槽! 进入使用状态 [InUse], 开始充能治疗..."))
                else
                    return NodeStatus.Failure
                end
            end

            -- 持续使用中，计时完成前保持 Running
            interactTimer = interactTimer + dt
            if interactTimer < interactDuration then
                return NodeStatus.Running
            end

            -- 使用完成: 回复生命值，释放智能对象插槽
            Health.Heal(aiAgentId, 60.0)
            SmartObject.Release(targetSmartObjectId)
            currentFsmState = FsmState.CoolingDown

            local curHp = Health.Get(aiAgentId).CurrentHealth
            print(string.format("[AI-BT] >> 充能完毕! 生命值恢复至 %.1f, 智能对象进入冷却 (Cooldown), NPC 恢复巡逻", curHp))

            targetSmartObjectId = nil
            currentFsmState = FsmState.Patrolling
            return NodeStatus.Success
        end),
    })

    -- 分支 B: 常规日常游荡巡逻 (低优先级 / 兜底行为)
    local defaultPatrolAction = CreateAction(function(dt)
        if currentFsmState ~= FsmState.Patrolling then
            return NodeStatus.Success
        end

        if not aiAgentEntity then
            return NodeStatus.Failure
        end

        local tr = aiAgentEntity:GetComponent("TransformComponent")
        if not tr then return NodeStatus.Failure end

        -- 在 [-3, 3] 区间往复巡逻
        local loc = tr.Location
        loc.x = loc.x + patrolDir * patrolSpeed * dt
        if loc.x > patrolRightLimit then
            loc.x = patrolRightLimit
            patrolDir = -1.0
        elseif loc.x < patrolLeftLimit then
            loc.x = patrolLeftLimit
            patrolDir = 1.0
        end
        tr.Location = loc

        return NodeStatus.Success
    end)

    -- 根节点 Selector: 优先检测急救，否则执行常规巡逻
    return CreateSelector({
        emergencyHealSequence,
        defaultPatrolAction,
    })
end

-- -----------------------------------------------------------------------------
-- 4. ECS 帧逻辑系统注册
-- -----------------------------------------------------------------------------
ecs:AddSystem("SmartObjectBehaviorDemoSystem", "Update", function(dt: number)
    -- 如果尚未绑定有效实体，尝试在当前场景中查找 AI Agent
    if not aiAgentEntity or not aiAgentId then
        local q = ecs:Query({ "TagComponent", "TransformComponent" })
        q:Each(function(e, tag, tr)
            if tag.Tag == "AI Agent" then
                aiAgentEntity = e
                aiAgentId = e:GetID()
            elseif tag.Tag == "Smart Health Station" then
                healthStationEntity = e
                healthStationId = e:GetID()
            elseif tag.Tag == "Smart Rest Bench" then
                restBenchEntity = e
                restBenchId = e:GetID()
            end
        end)

        -- 若当前场景无 AI Agent（如默认打开的关卡），静默早退，不执行 AI 逻辑，不抛任何异常
        if not aiAgentEntity or not aiAgentId then
            return
        end

        if not initialized then
            initialized = true
            print("[M3-AI-DEMO] ════════════════════════════════════════════════════════════════")
            print("[M3-AI-DEMO] 智能交互对象 (Smart Object) 与通用行为树 (BT) & 有限状态机 (FSM) 演示启动")
            print(string.format("[M3-AI-DEMO] 找到 AI Agent 实体 [ID: %d]", aiAgentId))
            if healthStationId then
                print(string.format("[M3-AI-DEMO] 找到 医疗充能站 [ID: %d]", healthStationId))
            end
            behaviorTreeRoot = BuildBehaviorTree()
            print("[M3-AI-DEMO] 行为树构建完成: [Selector (Root)] -> [Sequence (Low-HP Emergency)] / [Action (Patrol)]")
            print("[M3-AI-DEMO] 交互控制操作说明:")
            print("[M3-AI-DEMO]   [H 键] -> 对 AI Agent 施加 50 点伤害，将血量扣至 50 以下，触发紧急避险行为树分支")
            print("[M3-AI-DEMO]   [R 键] -> 重置 AI Agent 生命值为 100 满血")
            print("[M3-AI-DEMO] ════════════════════════════════════════════════════════════════")
        end
    end

    logTimer = logTimer + dt

    -- 2. 键盘交互响应 (检测 H 键扣血 / R 键重置)
    local hDown = false
    if Input.IsKeyPressed then
        hDown = Input.IsKeyPressed(72) or Input.IsKeyPressed("H")
    end
    if hDown and not lastHKey then
        if aiAgentId then
            local dealt = Health.Damage(aiAgentId, 50.0, 0)
            local info = Health.Get(aiAgentId)
            print(string.format("[M3-AI-DEMO] >>> [用户按下 H 键] 施加 %.1f 点伤害! Agent 当前剩余生命值: %.1f",
                dealt, info and info.CurrentHealth or 0.0))
        end
    end
    lastHKey = hDown

    local rDown = false
    if Input.IsKeyPressed then
        rDown = Input.IsKeyPressed(82) or Input.IsKeyPressed("R")
    end
    if rDown and not lastRKey then
        if aiAgentId then
            Health.Heal(aiAgentId, 100.0)
            local info = Health.Get(aiAgentId)
            print(string.format("[M3-AI-DEMO] >>> [用户按下 R 键] 生命值已重置满血: %.1f",
                info and info.CurrentHealth or 100.0))
        end
    end
    lastRKey = rDown

    -- 3. 运行行为树单帧求值
    if behaviorTreeRoot then
        behaviorTreeRoot:Tick(dt)
    end

    -- 4. 周期性状态 HUD 输出 (每 2.5 秒打印一次现场决策快照)
    if logTimer >= 2.5 then
        logTimer = 0.0
        local hpStr = "N/A"
        if aiAgentId then
            local info = Health.Get(aiAgentId)
            if info then
                hpStr = string.format("%.0f/%.0f", info.CurrentHealth, info.MaxHealth)
            end
        end

        local soStateStr = "Unknown"
        if healthStationId then
            local s = SmartObject.GetState(healthStationId)
            if s == 0 then soStateStr = "Available (空闲就绪)"
            elseif s == 1 then soStateStr = "Claimed (预约锁定)"
            elseif s == 2 then soStateStr = "InUse (正在使用)"
            elseif s == 3 then soStateStr = "Cooldown (冷却中)"
            end
        end

        print(string.format("[AI-HUD] NPC 状态: [%s] | 生命值: [%s] | 医疗站状态: [%s]",
            currentFsmState, hpStr, soStateStr))
    end
end)
