-- =============================================================================
-- WorldEngine — AI 行为动作脚本标准模版 (SampleCustomAction.luau)
-- =============================================================================
-- 【架构定位：AI 行为逻辑的三层架构规范 (Three-Tier AI Architecture)】
--
-- 1. 第一层：决策编排层 (Decision Graph in .wbt Editor)
--    - 所在位置：.wbt 资产专属工作台 (BehaviorTreeEditorPanel)。
--    - 核心职责：编排顶层决策流向（控制流节点、优先级分支选择与门禁拦截）。
--      例如：Selector (优先选择)、Sequence (顺次执行)、Parallel (并发执行)。
--    - 设计原则：行为树结构仅负责宏观调度“在何种时机、满足何种前置条件时执行何种动作 (What & When)”，
--      严禁在 .wbt 节点描述或树逻辑中堆砌底层物理位移、数学解算、寻路微操或动画渲染细节。
--
-- 2. 第二层：具体行为与谓词逻辑实现层 (Luau Action / Condition Scripts)
--    - 所在位置：项目工程 assets/scripts/ai/actions/ 与 assets/scripts/ai/conditions/。
--    - 核心职责：承载单一动作的具体业务逻辑（如向目标移动、巡逻路径点插值、播放前摇攻击动画、
--      与 SmartObject 交互握手、寻路朝向平滑插值等）。
--    - 协作规范：每个 Action 实现为独立的 Luau 模块，严格遵循标准生命周期回调规范：
--      - OnEnter(entity, btComponent, blackboard)
--      - OnTick(entity, btComponent, blackboard, dt) -> NodeStatus ("Running" | "Success" | "Failure")
--      - OnExit(entity, btComponent, blackboard, reason)
--    - 编辑器打通：在 .wbt 行为树编辑器中，动作类型下拉选择 [ScriptAction]，填入脚本路径
--      (如 assets/scripts/ai/actions/SampleCustomAction.luau)，点击旁边的 [</>] 按钮，
--      即可直接在内置代码编辑器 (ScriptEditorPanel) 中即时编写、调试并支持运行时热重载。
--
-- 3. 第三层：ECS 批量物理与帧系统推进层 (Pure ECS Frame Systems)
--    - 所在位置：assets/scripts/systems/ 或 C++ 引擎核心系统。
--    - 核心职责：负责全场景海量实体的批量数据推进、空间哈希与 NavMesh 批量更新、刚体碰撞解算与渲染同步。
--    - 解耦原则：Action 脚本通过标准接口读写实体的通用组件 (如 TransformComponent、HealthComponent、
--      BehaviorTreeComponent) 或调用全局服务 (如 Health、SmartObject、Navigation)，
--      与底层的批量 ECS 帧系统保持完全正交与解耦。
-- =============================================================================

local SampleCustomAction = {}

-- -----------------------------------------------------------------------------
-- 节点执行状态枚举 (严格对齐 World::Gameplay::NodeStatus 契约)
-- -----------------------------------------------------------------------------
SampleCustomAction.NodeStatus = {
    Success = "Success", -- 动作成功完成，通知上层 Sequence 顺次推进或 Selector 结束评估
    Failure = "Failure", -- 动作执行失败/受阻，通知上层流控节点执行失败回退或分支切换
    Running = "Running", -- 动作仍在进行中（如移动中、吟唱中），下一帧继续调用 OnTick 推进
}

-- -----------------------------------------------------------------------------
-- 动作元数据定义 (供行为树编辑器与 Inspector 属性检视器反射与智能提示)
-- -----------------------------------------------------------------------------
SampleCustomAction.Name = "SampleCustomAction"
SampleCustomAction.DisplayName = "移动并恢复生命值 (Move & Heal Action)"
SampleCustomAction.Description = "标准移动与生命恢复范例动作：向黑板目标坐标平滑移动，并在移动过程中持续恢复生命值"
SampleCustomAction.Category = "Combat / Movement"

-- 默认配置参数 (可在 .wbt 动作节点参数或黑板中按需覆盖)
SampleCustomAction.DefaultConfig = {
    MoveSpeed = 3.5,            -- 巡航移动速度 (m/s)
    ArrivalTolerance = 0.4,     -- 判定抵达目标的半径容差 (米)
    HealRate = 15.0,            -- 移动过程中的持续生命恢复速率 (点/秒)
    Timeout = 10.0,             -- 最大允许执行超时 (秒)，防止在障碍物前无限卡死
}

-- -----------------------------------------------------------------------------
-- 生命周期回调 1: OnEnter
-- -----------------------------------------------------------------------------
--- 在行为树控制流首次激活并进入本动作节点时触发调用一次。
--- 典型用途：初始化动作上下文、重置执行计时器、捕获/校验目标实体或坐标、播放起手音效与过渡动画。
---
---@param entity any 当前执行该行为树的实体句柄 (Entity)
---@param btComponent any 实体的 BehaviorTreeComponent (可读写 BlackboardTargetPos, ExecutionTimer 等)
---@param blackboard table 当前实体的黑板字典 (可通过键名读写动态属性，如 blackboard.TargetPos)
function SampleCustomAction.OnEnter(entity, btComponent, blackboard)
    -- 1. 重置行为树节点执行计时器
    if btComponent then
        btComponent.ExecutionTimer = 0.0
        -- 记录当前活跃状态机/动作状态标识 (0=Idle, 1=Moving, 2=Acting)
        btComponent.StateMachineState = 1
    end

    -- 2. 校验实体必要核心组件 (工业级防卫：绝不静默崩溃或抛出裸异常)
    if not entity or not entity:IsValid() then
        warn(string.format("[%s] OnEnter: 传入的 Entity 句柄无效!", SampleCustomAction.Name))
        return
    end

    local transform = entity:GetComponent("TransformComponent")
    if not transform then
        warn(string.format("[%s] OnEnter: Entity [ID: %s] 缺少 TransformComponent，动作将无法生效!",
            SampleCustomAction.Name, tostring(entity:GetID())))
        return
    end

    -- 3. 读取目标坐标 (支持高层 blackboard 动态表与 C1 原生紧凑黑板双通道)
    local targetPos = nil
    if blackboard and blackboard.MoveTargetPos then
        targetPos = blackboard.MoveTargetPos
    elseif btComponent and btComponent.BlackboardTargetPos then
        targetPos = btComponent.BlackboardTargetPos
    end

    -- 4. 若黑板配置了目标实体 (TargetEntity)，亦可动态同步该实体的最新世界坐标
    if btComponent and btComponent.TargetEntity and btComponent.TargetEntity ~= 0 then
        -- 如果目标是 SmartObject，可以通过 SmartObject 服务获取插槽坐标
        if SmartObject and SmartObject.GetPosition then
            local p = SmartObject:GetPosition(btComponent.TargetEntity)
            if p and p.x then
                targetPos = p
                btComponent.BlackboardTargetPos = p
            end
        end
    end

    -- print(string.format("[%s] OnEnter: 实体 %d 开始执行移动动作，初始目标: (%.2f, %.2f, %.2f)",
    --     SampleCustomAction.Name, entity:GetID(),
    --     targetPos and targetPos.x or 0.0,
    --     targetPos and targetPos.y or 0.0,
    --     targetPos and targetPos.z or 0.0))
end

-- -----------------------------------------------------------------------------
-- 生命周期回调 2: OnTick
-- -----------------------------------------------------------------------------
--- 只要动作处于 Running 状态，行为树主循环每帧都会调用此接口推进业务逻辑。
--- 典型用途：空间位移推进、角色朝向平滑插值、生命/能量数值回复、抵达判定与超时检测。
---
---@param entity any 当前实体句柄 (Entity)
---@param btComponent any 实体的 BehaviorTreeComponent
---@param blackboard table 当前实体的黑板字典
---@param dt number 本帧逻辑时间步长 (Delta Time, 秒)
---@return string NodeStatus ("Running" | "Success" | "Failure")
function SampleCustomAction.OnTick(entity, btComponent, blackboard, dt)
    local NodeStatus = SampleCustomAction.NodeStatus

    -- 1. 基础指针与状态防卫
    if not entity or not entity:IsValid() or not btComponent then
        return NodeStatus.Failure
    end

    local transform = entity:GetComponent("TransformComponent")
    if not transform then
        return NodeStatus.Failure
    end

    -- 2. 累计执行时长并进行超时检测 (超时作为受阻失败返回，由树结构执行重试或逃逸分支)
    local currentTimer = btComponent.ExecutionTimer + dt
    btComponent.ExecutionTimer = currentTimer

    local timeoutLimit = (blackboard and blackboard.MaxActionDuration) or SampleCustomAction.DefaultConfig.Timeout
    if currentTimer >= timeoutLimit then
        -- 记录调试日志并返回 Failure
        -- print(string.format("[%s] 实体 %d 移动超时 (%.2fs >= %.2fs)，动作宣告失败",
        --     SampleCustomAction.Name, entity:GetID(), currentTimer, timeoutLimit))
        return NodeStatus.Failure
    end

    -- 3. 解析当前巡航目标坐标
    local target = (blackboard and blackboard.MoveTargetPos) or btComponent.BlackboardTargetPos
    if not target then
        return NodeStatus.Failure
    end

    -- 4. 计算当前与目标的水平相对向量 (XZ 平面位移)
    local currentPos = transform.Location
    local dx = target.x - currentPos.x
    local dz = target.z - currentPos.z
    local distSq = dx * dx + dz * dz

    local tolerance = (blackboard and blackboard.ArrivalTolerance) or SampleCustomAction.DefaultConfig.ArrivalTolerance
    if distSq <= (tolerance * tolerance) then
        -- 判定成功抵达目标！
        -- 将自身状态置为就绪，返回 Success
        btComponent.StateMachineState = 0
        return NodeStatus.Success
    end

    -- 5. 执行水平运动推进 (演示直接更新 TransformComponent.Location)
    local dist = math.sqrt(distSq)
    local speed = (blackboard and blackboard.MoveSpeed) or SampleCustomAction.DefaultConfig.MoveSpeed
    local step = speed * dt
    if step > dist then
        step = dist
    end

    local dirX = dx / dist
    local dirZ = dz / dist
    transform.Location.x = transform.Location.x + dirX * step
    transform.Location.z = transform.Location.z + dirZ * step

    -- 6. 平滑计算并设置角色朝向 (通过 TransformComponent.Rotation 写入四元数)
    local yawRad = math.atan2(dirX, dirZ)
    if quat and quat.fromEuler then
        transform.Rotation = quat.fromEuler(0.0, yawRad * (180.0 / math.pi), 0.0)
    end

    -- 7. 演示协同读写 HealthComponent (在移动期间持续享受生命恢复增益)
    local health = entity:GetComponent("HealthComponent")
    if health and health.CurrentHealth and health.MaxHealth then
        if not health.IsDead and health.CurrentHealth < health.MaxHealth then
            local healStep = SampleCustomAction.DefaultConfig.HealRate * dt
            health.CurrentHealth = math.min(health.MaxHealth, health.CurrentHealth + healStep)
            -- 实时更新黑板通用数值 (供行为树编辑器 Live Debugger 面板监视)
            btComponent.BlackboardValue = health.CurrentHealth
        end
    end

    -- 8. 尚未抵达目的地，返回 Running 保持动作在下一帧继续执行
    return NodeStatus.Running
end

-- -----------------------------------------------------------------------------
-- 生命周期回调 3: OnExit
-- -----------------------------------------------------------------------------
--- 当动作正常完成 (返回 Success/Failure) 或被上层流控节点中断 (Abort/Interrupt) 时调用。
--- 典型用途：复位临时状态机标记、释放占用的 SmartObject 插槽、关闭循环特效/音效、恢复常规动画状态。
---
---@param entity any 当前实体句柄 (Entity)
---@param btComponent any 实体的 BehaviorTreeComponent
---@param blackboard table 当前实体的黑板字典
---@param reason string 退出原因 ("Success" | "Failure" | "Interrupted")
function SampleCustomAction.OnExit(entity, btComponent, blackboard, reason)
    -- 1. 复位行为树组件状态
    if btComponent then
        btComponent.StateMachineState = 0
        btComponent.ExecutionTimer = 0.0
    end

    -- 2. 清理黑板瞬态标志 (示例)
    if blackboard and blackboard.TransientNavPath then
        blackboard.TransientNavPath = nil
    end

    -- print(string.format("[%s] OnExit: 实体 %s 退出动作, 原因: %s",
    --     SampleCustomAction.Name,
    --     entity and tostring(entity:GetID()) or "Unknown",
    --     tostring(reason)))
end

return SampleCustomAction
