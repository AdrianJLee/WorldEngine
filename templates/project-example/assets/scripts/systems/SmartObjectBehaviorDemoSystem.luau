-- =============================================================================
-- M3 智能交互对象 (Smart Objects) 与通用行为树 (BT) & 有限状态机 (FSM) 演示系统
-- =============================================================================
-- 本系统遵循 WorldEngine 工业级 Pure-ECS 架构规范:
--   1. 决策状态与黑板数据 100% 存储于实体的 BehaviorTreeComponent 中:
--      - StateMachineState: 0 = Patrolling, 1 = SeekingSmartObject, 2 = Interacting, 3 = CoolingDown
--      - BlackboardTargetPos: 交互目标插槽世界坐标 (vec3)
--      - ExecutionTimer: 交互使用耗时计时器 (秒)
--      - TargetEntity: 目标 SmartObject 实体句柄
--   2. 彻底消灭闭包 Mock 树与模块级全局实体变量，所有实体数据通过 ecs:Query 驱动
--   3. 移动与交互逻辑直接读写 TransformComponent, HealthComponent 与 SmartObjectComponent
-- =============================================================================

-- -----------------------------------------------------------------------------
-- 1. 状态与配置常量
-- -----------------------------------------------------------------------------
local FsmState = {
    Patrolling = 0,
    SeekingSmartObject = 1,
    Interacting = 2,
    CoolingDown = 3,
}

local FsmStateNames = {
    [0] = "Patrolling (巡逻)",
    [1] = "SeekingSmartObject (寻路前往医疗站)",
    [2] = "Interacting (治疗充能中)",
    [3] = "CoolingDown (冷却就绪)",
}

-- 演示常量 (只读参数配置)
local PATROL_SPEED = 2.0
local PATROL_LEFT_LIMIT = -3.0
local PATROL_RIGHT_LIMIT = 3.0
local SEEK_SPEED = 3.0
local INTERACT_DURATION = 2.0
local ARRIVAL_DISTANCE = 0.4
local LOW_HEALTH_THRESHOLD = 60.0
local HEAL_AMOUNT = 60.0

-- 键盘交互与诊断定时器 (纯宿主事件/日志消抖，无实体 AI 业务状态)
local lastHKey = false
local lastRKey = false
local logTimer = 0.0
local printedStartup = false

-- -----------------------------------------------------------------------------
-- 2. Pure-ECS 帧逻辑系统注册
-- -----------------------------------------------------------------------------
ecs:AddSystem("SmartObjectBehaviorDemoSystem", "Update", function(dt: number)
    local agentQuery = ecs:Query({ "BehaviorTreeComponent", "TransformComponent" })
    if agentQuery:Count() == 0 then
        return
    end

    if not printedStartup then
        printedStartup = true
        print("[M3-AI-DEMO] ════════════════════════════════════════════════════════════════")
        print("[M3-AI-DEMO] 智能交互对象 (Smart Object) 与通用行为树 (BT) & 有限状态机 (FSM) 演示启动")
        print("[M3-AI-DEMO] 架构规范: 100% Pure ECS, 状态存入 BehaviorTreeComponent, 无全局闭包变量")
        print("[M3-AI-DEMO] 交互控制说明:")
        print("[M3-AI-DEMO]   [H 键] -> 对 AI Agent 施加 50 点伤害，触发低血量紧急避险行为分支")
        print("[M3-AI-DEMO]   [R 键] -> 重置 AI Agent 生命值为 100 满血")
        print("[M3-AI-DEMO] ════════════════════════════════════════════════════════════════")
    end

    -- 键盘交互响应 (检测 H 键扣血 / R 键重置)
    local hDown = false
    local rDown = false
    if Input and Input.IsKeyPressed then
        hDown = Input.IsKeyPressed(72) or Input.IsKeyPressed("H")
        rDown = Input.IsKeyPressed(82) or Input.IsKeyPressed("R")
    end

    if hDown and not lastHKey then
        agentQuery:Each(function(e, bt, tr)
            local agentId = e:GetID()
            local dealt = Health.Damage(agentId, 50.0, 0)
            local info = Health.Get(agentId)
            print(string.format("[M3-AI-DEMO] >>> [用户按下 H 键] 施加 %.1f 点伤害! Agent [ID: %d] 当前剩余生命值: %.1f",
                dealt, agentId, info and info.CurrentHealth or 0.0))
        end)
    end
    lastHKey = hDown

    if rDown and not lastRKey then
        agentQuery:Each(function(e, bt, tr)
            local agentId = e:GetID()
            Health.Heal(agentId, 100.0)
            local info = Health.Get(agentId)
            print(string.format("[M3-AI-DEMO] >>> [用户按下 R 键] Agent [ID: %d] 生命值已重置满血: %.1f",
                agentId, info and info.CurrentHealth or 100.0))
        end)
    end
    lastRKey = rDown

    -- 驱动带 BehaviorTreeComponent 的实体状态机推进
    agentQuery:Each(function(e, bt, tr)
        if bt.Enabled == 0 then
            return
        end

        local agentId = e:GetID()
        local healthInfo = Health.Get(agentId)
        local currentHealth = healthInfo and healthInfo.CurrentHealth or 100.0

        local state = bt.StateMachineState

        if state == FsmState.Patrolling then
            -- 分支 A: 检查低血量紧急避险条件
            if currentHealth < LOW_HEALTH_THRESHOLD then
                local bestSoId = nil
                local bestDistSq = 999999.0
                local bestSlotPos = nil

                local soQuery = ecs:Query({ "SmartObjectComponent", "TransformComponent" })
                soQuery:Each(function(soEntity, soComp, soTr)
                    local soId = soEntity:GetID()
                    local tagComp = soEntity:GetComponent("TagComponent")
                    local isHealthStation = (tagComp and tagComp.Tag == "Smart Health Station") 
                    if isHealthStation and soComp.Enabled then
                        local s = SmartObject.GetState(soId)
                        if s == 0 then -- 0 = Available
                            local pos = SmartObject.GetPosition(soId)
                            local dx = pos.x - tr.Location.x
                            local dz = pos.z - tr.Location.z
                            local distSq = dx * dx + dz * dz
                            if distSq < bestDistSq then
                                bestDistSq = distSq
                                bestSoId = soId
                                bestSlotPos = pos
                            end
                        end
                    end
                end)

                if bestSoId ~= nil and bestSlotPos ~= nil then
                    local claimed = SmartObject.Claim(bestSoId, agentId)
                    if claimed or SmartObject.GetState(bestSoId) == 1 then
                        bt.TargetEntity = bestSoId
                        bt.BlackboardTargetPos = vec3.new(bestSlotPos.x, bestSlotPos.y, bestSlotPos.z)
                        bt.StateMachineState = FsmState.SeekingSmartObject
                        bt.ExecutionTimer = 0.0
                        bt.Status = 1 -- Running
                        print(string.format("[AI-BT] >> 决策命中: 低血量紧急避险! 成功预约医疗站 [ID: %d], 槽位目标=(%.2f, %.2f, %.2f)",
                            bestSoId, bestSlotPos.x, bestSlotPos.y, bestSlotPos.z))
                        return
                    end
                end
            end

            -- 分支 B: 常规往复巡逻
            bt.StateMachineState = FsmState.Patrolling
            bt.TargetEntity = 0
            local dir = bt.BlackboardValue
            if dir ~= 1.0 and dir ~= -1.0 then
                dir = 1.0
            end

            local loc = tr.Location
            local newX = loc.x + dir * PATROL_SPEED * dt
            if newX > PATROL_RIGHT_LIMIT then
                newX = PATROL_RIGHT_LIMIT
                dir = -1.0
            elseif newX < PATROL_LEFT_LIMIT then
                newX = PATROL_LEFT_LIMIT
                dir = 1.0
            end
            bt.BlackboardValue = dir
            tr.Location = vec3.new(newX, loc.y, loc.z)
            bt.Status = 2 -- Success

        elseif state == FsmState.SeekingSmartObject then
            if bt.TargetEntity == 0 then
                bt.StateMachineState = FsmState.Patrolling
                bt.Status = 3 -- Failure
                return
            end

            local targetPos = bt.BlackboardTargetPos
            local loc = tr.Location
            local dx = targetPos.x - loc.x
            local dz = targetPos.z - loc.z
            local dist = math.sqrt(dx * dx + dz * dz)

            if dist > ARRIVAL_DISTANCE then
                local moveStep = SEEK_SPEED * dt
                local step = math.min(dist, moveStep)
                tr.Location = vec3.new(loc.x + (dx / dist) * step, loc.y, loc.z + (dz / dist) * step)
                bt.Status = 1 -- Running
            else
                -- 到达插槽位置，开始使用智能对象
                local used = SmartObject.Use(bt.TargetEntity, agentId)
                if used or SmartObject.GetState(bt.TargetEntity) == 2 then
                    bt.StateMachineState = FsmState.Interacting
                    bt.ExecutionTimer = 0.0
                    bt.Status = 1 -- Running
                    print("[AI-BT] >> 到达插槽! 进入使用状态 [InUse], 开始充能治疗...")
                else
                    bt.StateMachineState = FsmState.Patrolling
                    bt.TargetEntity = 0
                    bt.ExecutionTimer = 0.0
                    bt.Status = 3 -- Failure
                end
            end

        elseif state == FsmState.Interacting then
            bt.ExecutionTimer = bt.ExecutionTimer + dt
            if bt.ExecutionTimer < INTERACT_DURATION then
                bt.Status = 1 -- Running
            else
                -- 交互完成，生命值充能并释放智能对象
                Health.Heal(agentId, HEAL_AMOUNT)
                if bt.TargetEntity ~= 0 then
                    SmartObject.Release(bt.TargetEntity)
                end

                local curHp = 0.0
                local updatedInfo = Health.Get(agentId)
                if updatedInfo then
                    curHp = updatedInfo.CurrentHealth
                end
                print(string.format("[AI-BT] >> 充能完毕! 生命值恢复至 %.1f, 智能对象进入冷却 (Cooldown), NPC 恢复巡逻", curHp))

                bt.TargetEntity = 0
                bt.ExecutionTimer = 0.0
                bt.StateMachineState = FsmState.CoolingDown
                bt.Status = 2 -- Success
            end

        elseif state == FsmState.CoolingDown then
            -- 退出冷却状态，恢复常规巡逻
            bt.StateMachineState = FsmState.Patrolling
            bt.ExecutionTimer = 0.0
            bt.TargetEntity = 0
            bt.Status = 2 -- Success
        end
    end)

    -- 周期性状态 HUD 输出 (每 2.5 秒打印一次现场决策快照)
    logTimer = logTimer + dt
    if logTimer >= 2.5 then
        logTimer = 0.0

        local soStateStr = "N/A"
        local soQuery = ecs:Query({ "SmartObjectComponent", "TagComponent" })
        soQuery:Each(function(soEntity, soComp, tagComp)
            if tagComp.Tag == "Smart Health Station"  then
                local s = SmartObject.GetState(soEntity:GetID())
                if s == 0 then soStateStr = "Available (空闲就绪)"
                elseif s == 1 then soStateStr = "Claimed (预约锁定)"
                elseif s == 2 then soStateStr = "InUse (正在使用)"
                elseif s == 3 then soStateStr = "Cooldown (冷却中)"
                end
            end
        end)

        agentQuery:Each(function(e, bt, tr)
            local agentId = e:GetID()
            local hpStr = "N/A"
            local info = Health.Get(agentId)
            if info then
                hpStr = string.format("%.0f/%.0f", info.CurrentHealth, info.MaxHealth)
            end
            local stateStr = FsmStateNames[bt.StateMachineState] or string.format("Unknown(%d)", bt.StateMachineState)
            print(string.format("[AI-HUD] NPC [ID: %d] 状态: [%s] | 生命值: [%s] | 医疗站状态: [%s]",
                agentId, stateStr, hpStr, soStateStr))
        end)
    end
end)

ecs:AddTeardownSystem("SmartObjectBehaviorDemoTeardown", function()
    -- Pure ECS 架构: 状态由场景 ECS 组件持有，无需重置闭包变量
    lastHKey = false
    lastRKey = false
    logTimer = 0.0
    printedStartup = false
end)
