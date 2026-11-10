-- =============================================================================
-- WorldEngine — AI 条件谓词脚本标准模版 (SampleCustomCondition.luau)
-- =============================================================================
-- 【架构定位：行为树条件谓词的职责与规范 (Behavior Tree Condition Predicate)】
--
-- 1. 纯函数式与零副作用原则 (Pure & Side-Effect Free):
--    - 条件节点是行为树中的“门禁守卫 (Guard / Precondition)”，用于在选择器 (Selector)
--      寻找首个有效分支、或在顺次执行器 (Sequence) 继续推进后续步骤时进行快速合法性检验。
--    - 严禁在 Evaluate 评估函数中执行破坏性操作（如修改实体坐标、扣除生命值、
--      占用资源插槽或触发全局事件）；
--    - 仅执行纯只读的状态评估，严格返回 boolean 类型 (true = 满足条件放行, false = 不满足阻断)。
--
-- 2. 极致性能与确定性 (Fast, Microsecond-Level & Deterministic):
--    - 在包含动态中断机制 (如 Reactive Selector / Dynamic Decorator) 的高频行为树中，
--      每个思考周期可能评估数十次条件。
--    - 评估逻辑必须在微秒级内返回，优先使用距离平方、黑板标签比对或组件字段读取；
--    - 避免在此阶段执行全场景线性实体遍历、复杂 NavMesh 路径寻路求解或开销巨大的连续射线检测。
--
-- 3. 典型应用场景:
--    - 目标有效性检查 (Target Validity): 目标实体是否存在、是否活着、实体句柄是否失效；
--    - 空间范围与距离判定 (Range & Proximity Check): 敌人是否进入警觉半径、是否处于攻击射程内；
--    - 黑板状态与标签判定 (Blackboard Tags & States): 战术标签匹配、警报等级、警戒计时；
--    - 属性阈值门禁 (Attribute Thresholds): 生命值是否低于危险红线、弹药量是否归零。
--
-- 4. 编辑器工作流打通:
--    - 在 .wbt 行为树编辑器中，条件节点类型下拉选择 [ScriptCondition]；
--    - 条件参数填入脚本路径 (如 assets/scripts/ai/conditions/SampleCustomCondition.luau)；
--    - 点击旁边的 [</>] 按钮，即可直接在内置代码编辑器 (ScriptEditorPanel) 中即时编写与热重载。
-- =============================================================================

local SampleCustomCondition = {}

-- -----------------------------------------------------------------------------
-- 条件元数据定义 (供行为树编辑器与 Inspector 属性检视器反射与智能提示)
-- -----------------------------------------------------------------------------
SampleCustomCondition.Name = "SampleCustomCondition"
SampleCustomCondition.DisplayName = "目标有效性与警戒距离判定 (Target Validity & Distance Condition)"
SampleCustomCondition.Description = "标准条件谓词范例：检验黑板目标实体是否存活，且与自身距离是否位于警戒范围内"
SampleCustomCondition.Category = "Perception / Combat"

-- 默认判定参数 (可在 .wbt 条件节点参数或黑板中按需覆盖)
SampleCustomCondition.DefaultParams = {
    MaxDistance = 15.0,         -- 最大有效警戒距离 (米)
    CheckTargetAlive = true,    -- 是否必须校验目标处于存活状态
    HealthThreshold = 100.0,    -- 自身生命值门限判定 (可选)
}

-- -----------------------------------------------------------------------------
-- 核心评估接口: Evaluate
-- -----------------------------------------------------------------------------
--- 同步评估条件谓词是否成立。
---
---@param entity any 当前执行该行为树的实体句柄 (Entity)
---@param btComponent any 实体的 BehaviorTreeComponent
---@param blackboard table 当前实体的黑板字典 (包含自定义参数与运行时数据)
---@return boolean true 表示条件达成（通过门禁），false 表示条件不满足（阻断分支）
function SampleCustomCondition.Evaluate(entity, btComponent, blackboard)
    -- 1. 基础有效性防卫 (工业级健壮性保障)
    if not entity or not entity:IsValid() or not btComponent then
        return false
    end

    -- 2. 评估黑板全局/局部休眠状态 (演示黑板开关门禁)
    if blackboard and blackboard.IsAiDisabled == true then
        return false
    end

    -- 3. 评估目标实体有效性 (Target Validity Check)
    -- 在 WorldEngine ECS 中，0 与 0xFFFFFFFF (4294967295) 代表无效实体句柄
    local targetEntityId = btComponent.TargetEntity
    if not targetEntityId or targetEntityId == 0 or targetEntityId == 4294967295 then
        -- 亦可回退检查高层黑板字典中的目标引用
        if blackboard and blackboard.TargetEntityId and blackboard.TargetEntityId ~= 0 then
            targetEntityId = blackboard.TargetEntityId
        else
            -- 目标实体未指定，条件不满足
            return false
        end
    end

    -- 4. 评估自身空间组件 (TransformComponent)
    local selfTransform = entity:GetComponent("TransformComponent")
    if not selfTransform then
        return false
    end

    -- 5. 校验目标实体的生存状态 (Check Alive via Health Service)
    local checkAlive = (blackboard and blackboard.CheckTargetAlive)
    if checkAlive == nil then
        checkAlive = SampleCustomCondition.DefaultParams.CheckTargetAlive
    end

    if checkAlive and Health and Health.Get then
        local healthInfo = Health:Get(targetEntityId)
        -- 若目标具有生命组件且已阵亡，则条件直接判定为失败
        if healthInfo and healthInfo.IsDead then
            return false
        end
    end

    -- 6. 空间距离与范围判定 (Proximity Range Check)
    local maxDist = (blackboard and blackboard.AggroDistance)
        or (blackboard and blackboard.MaxDistance)
        or SampleCustomCondition.DefaultParams.MaxDistance

    local maxDistSq = maxDist * maxDist

    -- 优先获取目标实体的实际空间位置
    local targetPos = nil
    if SmartObject and SmartObject.GetPosition then
        -- 若目标是 SmartObject 插槽，优先读取插槽真实世界坐标
        local soPos = SmartObject:GetPosition(targetEntityId)
        if soPos and soPos.x then
            targetPos = soPos
        end
    end

    -- 若未能从服务获取，尝试通过黑板记录的目标坐标 (BlackboardTargetPos) 评估
    if not targetPos and btComponent.BlackboardTargetPos then
        targetPos = btComponent.BlackboardTargetPos
    end

    if targetPos then
        local selfPos = selfTransform.Location
        local dx = selfPos.x - targetPos.x
        local dy = selfPos.y - targetPos.y
        local dz = selfPos.z - targetPos.z

        -- 使用平方距离进行比对，避免开平方根 (math.sqrt) 开销，契合高频评估性能要求
        local distSq = dx * dx + dy * dy + dz * dz
        if distSq > maxDistSq then
            -- 超出警戒范围，条件不成立
            return false
        end
    end

    -- 7. 演示评估自身生命值状态 (Health Condition Gate)
    local selfHealth = entity:GetComponent("HealthComponent")
    if selfHealth and selfHealth.IsDead then
        -- 自身若已阵亡，阻断所有主动进攻行为
        return false
    end

    -- 8. 演示评估黑板数值或战术标签
    -- 例如：BlackboardTag == 0 代表允许执行常规战术
    if btComponent.BlackboardTag ~= 0 and blackboard and blackboard.StrictTagMatching then
        return false
    end

    -- 9. 所有前置安全与距离门禁均已通过
    return true
end

return SampleCustomCondition
