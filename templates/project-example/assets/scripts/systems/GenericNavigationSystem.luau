-- =============================================================================
-- WorldEngine — Generic 3D Navigation & Multi-Level Architectural Showcase System
-- =============================================================================
-- 本系统演示通用 3D 空间导航网格 (NavMesh) 与高阶空间感知能力:
--   1. 纯白盒立体建筑无地貌导航 (Zero Terrain): 40x40m 大厅 + 8级双阶台阶 + 2F阳台天桥 + 单向斜坡;
--   2. 双阶直跑楼梯自动平滑跨接 (Staircase Traversal: Y=0.0m 顺畅攀爬至 Y=3.0m);
--   3. 区域代价加权寻路权衡 (Cost Tradeoff: Cost=0.5 硬化道路 vs Cost=3.0 中庭泥潭);
--   4. 跨网格特殊跳跃通道 (Off-Mesh Link: 从二楼阳台边缘一跃而下直降一楼减震垫);
--   5. 通用空间漫游采样 (Random Wander Mode: 自动采样建筑各区域并穿梭其间);
--   6. 通用视线通视检测 (LoS Raycast: 检测立柱与隔墙对 AI 与地标间的视线遮挡);
--   7. 工业级交互式 WUI HUD 实时卡片渲染与按键交互。
-- =============================================================================

local SceneScope = nil
local okScope, scopeLib = pcall(function() return ecs:RequireLib("scene_scope") end)
if okScope and type(scopeLib) == "table" then
    SceneScope = scopeLib
end

-- 实体标签常量
local AGENT_TAG = "Mannequin Agent"
local CAMERA_TAG = "Generic Navigation Camera"
local HALL_TAG = "Ground Floor Hall"
local STAIR_TAG = "Grand Staircase"
local BALCONY_TAG = "Second Floor Balcony"
local MUD_TAG = "Courtyard Mud Puddle"
local LANDING_PAD_TAG = "Jump Drop Landing Pad"

-- 四大立体空间地标
local BEACONS = {
    {
        Name = "2nd Floor Balcony (二楼观景阳台)",
        Pos = vec3.new(20.0, 3.0, 24.0),
        Floor = "2F",
        Description = "二楼架空阳台，俯瞰一楼大厅，由西侧双阶直跑楼梯爬升 3m 到达",
    },
    {
        Name = "Courtyard Gate (后花园北门)",
        Pos = vec3.new(20.0, 0.0, 36.0),
        Floor = "1F",
        Description = "中庭北侧大门。AI 评估代价后绕行环形硬化道路(Cost=0.5)，避开中庭深水泥潭(Cost=3.0)",
    },
    {
        Name = "Jump Landing Pad (阳台跳跃着陆垫)",
        Pos = vec3.new(20.0, 0.0, 14.0),
        Floor = "1F",
        Description = "一楼阳台正下方的减震安全垫。触发跳跃点时，AI 从二楼阳台边缘一跃而下着陆",
    },
    {
        Name = "Entrance Lobby (入口大厅大本营)",
        Pos = vec3.new(20.0, 0.0, 4.0),
        Floor = "1F",
        Description = "一楼南侧迎宾大厅，开阔平整，也是 AI 巡航与漫游的出发大本营",
    },
}

-- 建筑障碍体定义 (用于视线通视 LoS 检测)
local OCCLUDERS = {
    { Name = "Pillar SW", Pos = vec3.new(11.0, 2.5, 11.0), Radius = 1.0 },
    { Name = "Pillar SE", Pos = vec3.new(29.0, 2.5, 11.0), Radius = 1.0 },
    { Name = "Pillar NW", Pos = vec3.new(11.0, 2.5, 29.0), Radius = 1.0 },
    { Name = "Pillar NE", Pos = vec3.new(29.0, 2.5, 29.0), Radius = 1.0 },
    { Name = "Partition West", X0 = 8.0, X1 = 18.0, Z = 31.0, HalfThick = 0.6 },
    { Name = "Partition East", X0 = 22.0, X1 = 32.0, Z = 31.0, HalfThick = 0.6 },
}

-- 运行时状态
local initialized = false
local inScope = nil
local agentEntity = nil
local agentId = nil

-- 控制与状态参数
local currentTargetIndex = 1 -- 默认目标: 二楼阳台
local agentStateText = "Initializing"
local lastEventText = "关卡就绪，按 [1..4] 导航，按 [3] 跳跃，按 [W] 漫游，按 [R] 视线通视"
local lastEventTimer = 5.0

-- 自由漫游模式状态
local wanderActive = false
local wanderTimer = 0.0
local wanderTarget = vec3.new(20.0, 0.0, 4.0)

-- 阳台跳跃行动状态机 (Jump State Machine)
local jumpingActive = false
local jumpProgress = 0.0
local jumpStartPos = vec3.new(20.0, 3.0, 17.5)
local jumpEndPos = vec3.new(20.0, 0.0, 14.0)

-- 视线通视结果缓存
local losResults = {}
local losLastCheckTime = 0.0

-- 键盘防抖
local lastKey1 = false
local lastKey2 = false
local lastKey3 = false
local lastKey4 = false
local lastKeyW = false
local lastKeyR = false

-- 辅助：点到线段最近距离计算 (2D XZ)
local function DistPointToSegment2D(px, pz, ax, az, bx, bz)
    local abx = bx - ax
    local abz = bz - az
    local lenSq = abx * abx + abz * abz
    if lenSq < 1e-6 then
        local dx = px - ax
        local dz = pz - az
        return math.sqrt(dx * dx + dz * dz)
    end
    local t = math.max(0.0, math.min(1.0, ((px - ax) * abx + (pz - az) * abz) / lenSq))
    local closeX = ax + t * abx
    local closeZ = az + t * abz
    local dx = px - closeX
    local dz = pz - closeZ
    return math.sqrt(dx * dx + dz * dz)
end

-- 视线通视光线投射 (LoS Raycast)
local function PerformRaycast(startPos, endPos)
    for _, occ in ipairs(OCCLUDERS) do
        if occ.Radius then
            -- 圆柱体碰撞测试 (立柱)
            local dist = DistPointToSegment2D(occ.Pos.x, occ.Pos.z, startPos.x, startPos.z, endPos.x, endPos.z)
            if dist <= occ.Radius then
                return false, occ.Name, vec3.new(occ.Pos.x, 1.5, occ.Pos.z)
            end
        elseif occ.X0 then
            -- 墙体求交 (North Partition Walls at Z=31.0)
            local zLine = occ.Z
            -- 检查线段是否穿过 Z = zLine
            if (startPos.z - zLine) * (endPos.z - zLine) <= 0.0 and math.abs(endPos.z - startPos.z) > 1e-4 then
                local t = (zLine - startPos.z) / (endPos.z - startPos.z)
                local hitX = startPos.x + t * (endPos.x - startPos.x)
                if hitX >= occ.X0 and hitX <= occ.X1 then
                    return false, occ.Name, vec3.new(hitX, 1.5, zLine)
                end
            end
        end
    end
    return true, "CLEAR", endPos
end

-- 评估当前代理所在表面类型
local function EvaluateSurfaceType(pos)
    if pos.y >= 2.2 then
        if pos.z >= 20.0 and pos.x >= 16.0 and pos.x <= 24.0 then
            return "Balcony Deck (2F 观景阳台)"
        else
            return "Mezzanine Bridge (2F 架空天桥)"
        end
    elseif pos.y > 0.3 then
        if pos.x <= 8.5 and pos.z >= 7.5 and pos.z <= 16.5 then
            return "Grand Staircase (楼梯台阶爬升)"
        elseif pos.x >= 31.5 and pos.z >= 13.5 and pos.z <= 26.5 then
            return "East Descent Ramp (东侧斜坡引道)"
        else
            return "Elevated Platform"
        end
    else
        -- Ground level (1F)
        if pos.x >= 14.0 and pos.x <= 26.0 and pos.z >= 14.0 and pos.z <= 26.0 then
            return "Mud Puddle (中庭泥潭, Cost: 3.0)"
        elseif (pos.x >= 4.0 and pos.x <= 8.0) or (pos.x >= 32.0 and pos.x <= 36.0) then
            return "Paved Avenue (硬化环道, Cost: 0.5)"
        elseif pos.x >= 18.0 and pos.x <= 22.0 and pos.z >= 12.0 and pos.z <= 16.0 then
            return "Landing Pad (减震软垫)"
        else
            return "Ground Hall Floor (标准地面, Cost: 1.0)"
        end
    end
end

-- 设置寻路代理目标点
local function SetTarget(targetIdx: number)
    if targetIdx < 1 or targetIdx > #BEACONS then
        return
    end
    jumpingActive = false
    currentTargetIndex = targetIdx
    local target = BEACONS[currentTargetIndex]

    if agentEntity then
        local q = ecs:Query({ "TagComponent", "NavMeshAgentComponent" })
        q:Each(function(e, tag, agent)
            if tag.Tag == AGENT_TAG then
                agent.Destination = target.Pos
                agent.ReachedDestination = 0
                agent.AutoRepath = 1
            end
        end)
    end

    lastEventText = string.format("【目标指派】前往 [%d] %s", currentTargetIndex, target.Name)
    lastEventTimer = 3.5
    print(string.format("[GENERIC-NAV] 目标已设定 -> [%d] %s: 坐标 (%.1f, %.1f, %.1f)",
        currentTargetIndex, target.Name, target.Pos.x, target.Pos.y, target.Pos.z))
end

-- 触发阳台跳跃点 (Balcony Jump Drop)
local function TriggerBalconyJump()
    local agentPos = vec3.new(20.0, 0.0, 4.0)
    local q = ecs:Query({ "TagComponent", "TransformComponent" })
    q:Each(function(e, tag, tr)
        if tag.Tag == AGENT_TAG then
            agentPos = tr.Location
        end
    end)

    if agentPos.y >= 2.0 then
        -- 已经在二楼，直接前往阳台边缘并起跳
        jumpingActive = true
        jumpProgress = 0.0
        jumpStartPos = vec3.new(agentPos.x, agentPos.y, agentPos.z)
        jumpEndPos = BEACONS[3].Pos
        currentTargetIndex = 3
        lastEventText = "【阳台高空跳跃】AI 从二楼阳台纵身一跃直落一楼减震垫！"
        lastEventTimer = 4.0
        print("[GENERIC-NAV] >>> 阳台直下跳跃启动: 抛物线垂直高差 3.0m 下降！")
    else
        -- 在一楼，先导航至二楼阳台
        SetTarget(1)
        lastEventText = "【引导登高】AI 正沿楼梯登上二楼阳台以准备跳跃..."
        lastEventTimer = 3.5
        print("[GENERIC-NAV] AI 正在一楼，先导航至二楼阳台准备起跳")
    end
end

-- 重置代理回入口大厅大本营
local function ResetAgentToBase()
    jumpingActive = false
    wanderActive = false
    local basePos = BEACONS[4].Pos
    local q = ecs:Query({ "TagComponent", "TransformComponent", "NavMeshAgentComponent" })
    q:Each(function(e, tag, tr, agent)
        if tag.Tag == AGENT_TAG then
            tr.Location = basePos
            agent.Destination = BEACONS[1].Pos
            agent.ReachedDestination = 0
        end
    end)
    currentTargetIndex = 1
    lastEventText = "【位置重置】AI 已重置回大本营 (20.0, 0.0, 4.0)"
    lastEventTimer = 3.5
    print("[GENERIC-NAV] AI 代理已重置回入口大厅")
end

-- 执行视线通视走查 (LoS Raycast Check)
local function CheckLineOfSight()
    local agentPos = vec3.new(20.0, 0.0, 4.0)
    local q = ecs:Query({ "TagComponent", "TransformComponent" })
    q:Each(function(e, tag, tr)
        if tag.Tag == AGENT_TAG then
            agentPos = tr.Location
        end
    end)

    losResults = {}
    print("--------------------------------------------------------------------------------")
    print(string.format("[GENERIC-NAV] 全场景视线通视 (LoS Raycast) 空间检测: 起点 (%.1f, %.1f, %.1f)",
        agentPos.x, agentPos.y, agentPos.z))
    for i, beacon in ipairs(BEACONS) do
        local clear, blocker, hitPt = PerformRaycast(agentPos, beacon.Pos)
        losResults[i] = { Clear = clear, Blocker = blocker, HitPt = hitPt }
        local status = clear and "✓ 通视良好 (CLEAR)" or string.format("✗ 被 [%s] 遮挡 (BLOCKED)", blocker)
        print(string.format("  -> 目标 [%d] %s: %s", i, beacon.Name, status))
    end
    print("--------------------------------------------------------------------------------")
    lastEventText = "【视线通视已检测】控制台已输出四大地标的射线相交结果"
    lastEventTimer = 3.5
end

-- 随机漫游采样逻辑 (Wander Mode)
local function UpdateWander(dt, currentPos)
    wanderTimer = wanderTimer + dt
    if wanderTimer >= 4.0 then
        wanderTimer = 0.0
        -- 随机选取建筑内的一个多边形区域点
        local randomChoice = math.random(1, 6)
        local nextWanderPt = vec3.new(20.0, 0.0, 4.0)

        if randomChoice == 1 then
            -- 1F 入口大厅附近
            nextWanderPt = vec3.new(math.random(10, 30), 0.0, math.random(4, 10))
        elseif randomChoice == 2 then
            -- 1F 西侧硬化大道
            nextWanderPt = vec3.new(6.0, 0.0, math.random(8, 32))
        elseif randomChoice == 3 then
            -- 1F 东侧硬化大道
            nextWanderPt = vec3.new(34.0, 0.0, math.random(8, 32))
        elseif randomChoice == 4 then
            -- 2F 观景阳台
            nextWanderPt = vec3.new(math.random(18, 22), 3.0, math.random(21, 25))
        elseif randomChoice == 5 then
            -- 2F 架空天桥西侧
            nextWanderPt = vec3.new(math.random(6, 14), 3.0, math.random(17, 19))
        else
            -- 1F 北侧花园门
            nextWanderPt = vec3.new(20.0, 0.0, 36.0)
        end

        wanderTarget = nextWanderPt
        local q = ecs:Query({ "TagComponent", "NavMeshAgentComponent" })
        q:Each(function(e, tag, agent)
            if tag.Tag == AGENT_TAG then
                agent.Destination = wanderTarget
                agent.ReachedDestination = 0
            end
        end)
        lastEventText = string.format("【自由漫游】采样新目标 (%.1f, %.1f, %.1f)",
            wanderTarget.x, wanderTarget.y, wanderTarget.z)
        lastEventTimer = 2.5
        print(string.format("[GENERIC-NAV] Wander Mode 采样新航点 -> (%.1f, %.1f, %.1f)",
            wanderTarget.x, wanderTarget.y, wanderTarget.z))
    end
end

-- 主逻辑帧系统
ecs:AddSystem("GenericNavigationSystem", "Update", function(dt: number)
    -- 1. 关卡范围守卫检查
    if inScope == nil then
        if SceneScope and type(SceneScope.owns) == "function" then
            inScope = SceneScope.owns(AGENT_TAG) or SceneScope.owns(CAMERA_TAG) or SceneScope.owns(HALL_TAG)
        else
            local found = false
            local q = ecs:Query({ "TagComponent" })
            q:Each(function(_e, tag)
                if tag.Tag == AGENT_TAG or tag.Tag == CAMERA_TAG or tag.Tag == HALL_TAG then
                    found = true
                end
            end)
            inScope = found
        end
    end
    if not inScope then
        return
    end

    -- 2. 查找并绑定 AI 实体
    if not agentEntity then
        local q = ecs:Query({ "TagComponent", "TransformComponent" })
        q:Each(function(e, tag, _tr)
            if tag.Tag == AGENT_TAG then
                agentEntity = e
                agentId = e:GetID()
            end
        end)

        if not agentEntity then
            return
        end

        if not initialized then
            initialized = true
            print("================================================================================")
            print("  WorldEngine 通用化 3D 导航网格与空间感知展示系统 (Generic NavMesh) 已启动")
            print("  ------------------------------------------------------------------------------")
            print("  纯白盒立体空间: 40x40m 大厅 + 8级双阶台阶(0->3m) + 2F阳台 + 单向斜坡")
            print("  快捷键指南:")
            print("    [1] 键 -> 前往二楼阳台 (2F Balcony) — 演示双阶台阶自动跨接爬升 3m")
            print("    [2] 键 -> 前往后花园门 (Courtyard Gate) — 演示代价权衡: 走道路(0.5)绕泥潭(3.0)")
            print("    [3] 键 -> 触发阳台跳跃 (Balcony Jump) — 演示从 3m 高台纵身一跃落入软垫")
            print("    [4] 键 -> 返回入口大厅 (Entrance Lobby) — 演示回归大本营")
            print("    [W] 键 -> 开启 / 关闭自由随机漫游模式 (Random Wander Mode)")
            print("    [R] 键 -> 执行视线通视射线检测 (Line-of-Sight Raycast)")
            print("================================================================================")

            SetTarget(1)
            CheckLineOfSight()
        end
    end

    -- 3. 倒计时器
    if lastEventTimer > 0.0 then
        lastEventTimer = math.max(0.0, lastEventTimer - dt)
    end

    -- 4. 键盘交互响应
    -- [1] 键
    local k1 = Input.IsKeyPressed and (Input.IsKeyPressed(49) or Input.IsKeyPressed("1")) or false
    if k1 and not lastKey1 then
        wanderActive = false
        SetTarget(1)
    end
    lastKey1 = k1

    -- [2] 键
    local k2 = Input.IsKeyPressed and (Input.IsKeyPressed(50) or Input.IsKeyPressed("2")) or false
    if k2 and not lastKey2 then
        wanderActive = false
        SetTarget(2)
    end
    lastKey2 = k2

    -- [3] 键
    local k3 = Input.IsKeyPressed and (Input.IsKeyPressed(51) or Input.IsKeyPressed("3")) or false
    if k3 and not lastKey3 then
        wanderActive = false
        TriggerBalconyJump()
    end
    lastKey3 = k3

    -- [4] 键
    local k4 = Input.IsKeyPressed and (Input.IsKeyPressed(52) or Input.IsKeyPressed("4")) or false
    if k4 and not lastKey4 then
        wanderActive = false
        SetTarget(4)
    end
    lastKey4 = k4

    -- [W] 键 (漫游模式切换)
    local kW = Input.IsKeyPressed and (Input.IsKeyPressed(87) or Input.IsKeyPressed("W") or Input.IsKeyPressed("w")) or false
    if kW and not lastKeyW then
        wanderActive = not wanderActive
        if wanderActive then
            wanderTimer = 3.5
            lastEventText = "【漫游模式已开启】AI 开始在建筑内自主漫步探索"
            print("[GENERIC-NAV] 自由漫游模式开启 (Wander Active)")
        else
            lastEventText = "【漫游模式已关闭】AI 恢复定点导航"
            print("[GENERIC-NAV] 自由漫游模式关闭")
        end
        lastEventTimer = 3.0
    end
    lastKeyW = kW

    -- [R] 键 (通视检测)
    local kR = Input.IsKeyPressed and (Input.IsKeyPressed(82) or Input.IsKeyPressed("R") or Input.IsKeyPressed("r")) or false
    if kR and not lastKeyR then
        CheckLineOfSight()
    end
    lastKeyR = kR

    -- 5. 阳台高空跳跃物理插值动画
    if jumpingActive then
        jumpProgress = jumpProgress + dt * 1.5
        local t = math.min(1.0, jumpProgress)
        local currentX = jumpStartPos.x + t * (jumpEndPos.x - jumpStartPos.x)
        local currentZ = jumpStartPos.z + t * (jumpEndPos.z - jumpStartPos.z)
        -- 垂直跳跃下抛物线
        local currentY = jumpStartPos.y * (1.0 - t) + 1.2 * math.sin(t * math.pi) * (1.0 - t)

        local q = ecs:Query({ "TagComponent", "TransformComponent" })
        q:Each(function(e, tag, tr)
            if tag.Tag == AGENT_TAG then
                tr.Location = vec3.new(currentX, currentY, currentZ)
            end
        end)

        agentStateText = "Jumping Off Balcony (高空跳跃中)"
        if t >= 1.0 then
            jumpingActive = false
            lastEventText = "【安全着陆】AI 已平稳落在一楼减震软垫上！"
            lastEventTimer = 3.5
            print("[GENERIC-NAV] 跳跃完成，安全着陆于软垫")
        end
        return
    end

    -- 6. 寻路状态与漫游更新
    local qAgent = ecs:Query({ "TagComponent", "TransformComponent", "NavMeshAgentComponent" })
    qAgent:Each(function(e, tag, tr, agent)
        if tag.Tag == AGENT_TAG then
            local currentPos = tr.Location
            local targetPos = wanderActive and wanderTarget or BEACONS[currentTargetIndex].Pos
            local dist = math.sqrt((targetPos.x - currentPos.x)^2 + (targetPos.y - currentPos.y)^2 + (targetPos.z - currentPos.z)^2)

            if wanderActive then
                UpdateWander(dt, currentPos)
                agentStateText = "Wandering (自由漫游探索中)"
            else
                if agent.ReachedDestination == 1 or dist <= agent.StoppingDistance then
                    agentStateText = "Arrived (已抵达目标)"
                elseif currentPos.y > 0.3 and currentPos.y < 2.5 and currentPos.x <= 9.0 then
                    agentStateText = "Climbing Stairs (台阶攀爬中 3m)"
                elseif currentPos.y > 0.3 and currentPos.y < 2.5 and currentPos.x >= 31.0 then
                    agentStateText = "Descending Ramp (斜坡下行中)"
                else
                    agentStateText = "Navigating (导航寻路中)"
                end
            end
        end
    end)
end)

-- 7. 交互式 WUI HUD 实时卡片渲染
ui.onDraw(function()
    if not inScope or not initialized then
        return
    end

    -- 获取当前代理状态数据
    local currentPos = vec3.new(20.0, 0.0, 4.0)
    local targetPos = wanderActive and wanderTarget or BEACONS[currentTargetIndex].Pos
    local distRemaining = 0.0
    local speed = 4.5

    local qAgent = ecs:Query({ "TagComponent", "TransformComponent", "NavMeshAgentComponent" })
    qAgent:Each(function(e, tag, tr, agent)
        if tag.Tag == AGENT_TAG then
            currentPos = tr.Location
            speed = agent.Speed
            local dx = targetPos.x - currentPos.x
            local dy = targetPos.y - currentPos.y
            local dz = targetPos.z - currentPos.z
            distRemaining = math.sqrt(dx * dx + dy * dy + dz * dz)
        end
    end)

    local floorStr = currentPos.y >= 2.0 and "【2F 架空阳台/天桥 (3.0m)】" or "【1F 大厅地面 (0.0m)】"
    local surfaceStr = EvaluateSurfaceType(currentPos)

    -- 绘制主面板
    ui.panel(24, 24, 430, 520, "Generic 3D Navigation & Perception Showcase")

    -- 状态与指标
    ui.text(40, 56, "楼层位置   " .. floorStr, 14)
    ui.text(40, 76, "地表类型   " .. surfaceStr, 13)
    ui.text(40, 96, "寻路状态   " .. agentStateText, 13)
    ui.text(40, 116, string.format("代理坐标   (%.1f, %.1f, %.1f)", currentPos.x, currentPos.y, currentPos.z), 13)

    local targetNameStr = wanderActive and string.format("漫游航点 (%.1f, %.1f, %.1f)", wanderTarget.x, wanderTarget.y, wanderTarget.z)
        or string.format("[%d] %s", currentTargetIndex, BEACONS[currentTargetIndex].Name)
    ui.text(40, 136, "当前目标   " .. targetNameStr, 13)
    ui.text(40, 156, string.format("剩余距离   %.2f m    巡航速度: %.1f m/s", distRemaining, speed), 13)

    -- 漫游与视线通视状态
    local wanderStatusStr = wanderActive and "【开启 ON】(多层建筑自由漫步)" or "【关闭 OFF】(定点指令导航)"
    ui.text(40, 178, "自由漫游   " .. wanderStatusStr, 13)

    local losSummary = losResults[currentTargetIndex] and (losResults[currentTargetIndex].Clear and "通畅 CLEAR" or ("阻断 BLOCKED (" .. losResults[currentTargetIndex].Blocker .. ")")) or "待检测"
    ui.text(40, 198, "视线通视   与当前目标: " .. losSummary, 13)

    -- 快捷按键提示
    ui.text(40, 222, "快捷键: [1..4] 目标点   [3] 阳台跳跃   [W] 漫游   [R] 通视检测", 12)

    -- 交互按钮组
    if ui.button("btn_target_1", 40, 244, 90, 26, "[1] 2F阳台") then
        wanderActive = false
        SetTarget(1)
    end
    if ui.button("btn_target_2", 135, 244, 90, 26, "[2] 后花园") then
        wanderActive = false
        SetTarget(2)
    end
    if ui.button("btn_target_3", 230, 244, 90, 26, "[3] 跳跃点") then
        wanderActive = false
        TriggerBalconyJump()
    end
    if ui.button("btn_target_4", 325, 244, 90, 26, "[4] 入口大厅") then
        wanderActive = false
        SetTarget(4)
    end

    local wanderBtnText = wanderActive and "停止自由漫游 (切换为定点停留)" or "开启多层自由漫游 (Wander Mode)"
    if ui.button("btn_toggle_wander", 40, 276, 375, 28, wanderBtnText) then
        wanderActive = not wanderActive
        wanderTimer = 3.5
        lastEventText = wanderActive and "【已开启自由漫游】" or "【已停止自由漫游】"
        lastEventTimer = 3.0
    end

    if ui.button("btn_check_los", 40, 310, 375, 28, "执行视线通视检测 (LoS Raycast Test)") then
        CheckLineOfSight()
    end

    if ui.button("btn_reset_agent", 40, 344, 375, 28, "重置 AI 位置 (返回大本营)") then
        ResetAgentToBase()
    end

    -- 动态操作提示文字
    ui.text(40, 382, "最新操作: " .. lastEventText, 11)

    -- 机制原理解析
    ui.text(40, 408, "通用 3D 导航网格系统特性解析:", 12)
    ui.text(40, 426, "- 纯白盒零地形: 全场景几何体通用烘焙，支持立体多层空间", 10)
    ui.text(40, 442, "- 台阶自动跨接: 8级直跑楼梯平滑爬升至 3.0m 高台天桥", 10)
    ui.text(40, 458, "- 代价加权选路: 自动避让泥潭 (Cost=3.0)，绕行硬道 (Cost=0.5)", 10)
    ui.text(40, 474, "- 跨网格高空跳跃: Off-Mesh Link 赋能阳台直降与特技通道", 10)
    ui.text(40, 490, "- 空间感知查询: 视线通视 (Raycast) 与随机漫游 (RandomPoint)", 10)
end)

-- 8. 场景卸载清理系统
ecs:AddTeardownSystem("GenericNavigationTeardown", function()
    initialized = false
    inScope = nil
    agentEntity = nil
    agentId = nil
    wanderActive = false
    jumpingActive = false
    print("[GENERIC-NAV] 演示系统已安全执行收尾清理 (Teardown)")
end)

print("[PureECS] GenericNavigationSystem 与 TeardownSystem 已注册至场景生命周期")
