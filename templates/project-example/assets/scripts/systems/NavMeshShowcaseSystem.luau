-- =============================================================================
-- WorldEngine — 3D NavMesh & AI Navigation Showcase System
-- =============================================================================
-- 本系统演示纯 3D 空间导航网格 (NavMesh) 与智能寻路代理的核心能力:
--   1. 3D A* 凸多边形走廊搜索 + Funnel (String-Pulling) 漏斗平滑路径规划;
--   2. 地貌高度场自适应烘焙与坡度剔除 (平缓坡道 <=25° 通行，陡峭悬崖 >=50° 阻断);
--   3. 动态障碍物 (NavMeshObstacleComponent) 实时打洞雕刻 (Carving);
--   4. 毫秒级动态重新寻路 (Dynamic Repathing) 绕行展示;
--   5. 多地标点 (Landmark Beacons) 四点循环巡逻与实时交互式 WUI 状态 HUD。
-- =============================================================================

local SceneScope = nil
local okScope, scopeLib = pcall(function() return ecs:RequireLib("scene_scope") end)
if okScope and type(scopeLib) == "table" then
    SceneScope = scopeLib
end

-- 实体标签常量
local AGENT_TAG = "Mannequin Agent"
local BARRICADE_TAG = "Valley Barricade"
local CAMERA_TAG = "NavMesh Showcase Camera"
local TERRAIN_TAG = "Showcase Terrain"

-- 四大地标坐标定义
local BEACONS = {
    {
        Name = "Plateau Hilltop (高台顶峰)",
        Pos = vec3.new(16.0, 5.5, 32.0),
        Description = "位于西侧高台顶端，需经由南侧平缓坡道爬升到达",
    },
    {
        Name = "Valley Exit (峡谷远端)",
        Pos = vec3.new(32.0, 0.0, 52.0),
        Description = "位于峡谷北侧出口，常态下走峡谷直通，路障升起时需绕行高台",
    },
    {
        Name = "Rocky Outpost (碎石前哨)",
        Pos = vec3.new(48.0, 2.5, 32.0),
        Description = "位于东侧山麓台地，经碎石步道连通大本营与峡谷出口",
    },
    {
        Name = "Base Camp (大本营营地)",
        Pos = vec3.new(32.0, 0.0, 12.0),
        Description = "南侧平坦开阔地，也是寻路代理的初始出发基地",
    },
}

-- 运行时状态
local initialized = false
local inScope = nil

local agentEntity = nil
local agentId = nil
local barricadeEntity = nil
local barricadeId = nil

-- 代理控制参数
local currentTargetIndex = 2 -- 默认目标: 峡谷远端
local barricadeClosed = true -- 默认路障升起阻挡
local patrolActive = false
local patrolTimer = 0.0

-- 状态显示缓存
local agentStateText = "Initializing"
local rerouteTimer = 0.0
local lastEventText = "关卡就绪，请按下 [1..4] 指定目标，或按 [Space] 切换峡谷路障"
local lastEventTimer = 5.0

-- 键盘防抖状态
local lastKey1 = false
local lastKey2 = false
local lastKey3 = false
local lastKey4 = false
local lastKeySpace = false
local lastKeyO = false
local lastKeyP = false
local lastKeyC = false

-- 辅助安全设置旋转朝向
local function setRotation(transform, xDeg, yDeg, zDeg)
    if type(quat) == "nil" or type(quat.fromEuler) ~= "function" then
        return false
    end
    local ok = pcall(function()
        transform.Rotation = quat.fromEuler(xDeg, yDeg, zDeg)
    end)
    return ok
end

-- 切换路障升降阻隔
local function SetBarricade(closed: boolean)
    barricadeClosed = closed
    if not barricadeEntity then
        return
    end

    local q = ecs:Query({ "TagComponent", "TransformComponent", "NavMeshObstacleComponent" })
    q:Each(function(e, tag, tr, obs)
        if tag.Tag == BARRICADE_TAG then
            if barricadeClosed then
                obs.CarveNavMesh = 1
                tr.Location = vec3.new(32.0, 1.0, 32.0)
            else
                obs.CarveNavMesh = 0
                tr.Location = vec3.new(32.0, -2.5, 32.0)
            end
        end
    end)

    -- 同步刷新路障外观颜色 (红=阻断，绿=通行)
    local qMesh = ecs:Query({ "TagComponent", "MeshRendererComponent" })
    qMesh:Each(function(e, tag, mr)
        if tag.Tag == BARRICADE_TAG then
            if barricadeClosed then
                mr.Color = vec4.new(0.85, 0.22, 0.15, 1.0)
            else
                mr.Color = vec4.new(0.2, 0.85, 0.35, 1.0)
            end
        end
    end)

    rerouteTimer = 2.0
    lastEventTimer = 4.0
    if barricadeClosed then
        lastEventText = "【路障升起 [已阻断]】中央峡谷通道关闭，AI 代理立即检测并重新规划绕行高台！"
        print("[NAVMESH-SHOWCASE] >>> 峡谷路障升起 (CLOSED): 触发 NavMeshObstacle 雕刻打洞，代理开始动态重规划")
    else
        lastEventText = "【路障降下 [可通行]】中央峡谷通道畅通，AI 代理选择最近直达捷径！"
        print("[NAVMESH-SHOWCASE] >>> 峡谷路障降下 (OPEN): 移除 NavMesh 雕刻阻挡，直行通道恢复")
    end
end

-- 设置寻路代理目标点
local function SetTarget(targetIdx: number)
    if targetIdx < 1 or targetIdx > #BEACONS then
        return
    end
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

    lastEventText = string.format("【目标已指派】前往 [%d] %s", currentTargetIndex, target.Name)
    lastEventTimer = 3.5
    print(string.format("[NAVMESH-SHOWCASE] 目标更新 -> [%d] %s: 坐标 (%.1f, %.1f, %.1f)",
        currentTargetIndex, target.Name, target.Pos.x, target.Pos.y, target.Pos.z))
end

-- 重置代理回大本营
local function ResetAgentToBaseCamp()
    if not agentEntity then
        return
    end
    local campPos = BEACONS[4].Pos
    local q = ecs:Query({ "TagComponent", "TransformComponent", "NavMeshAgentComponent" })
    q:Each(function(e, tag, tr, agent)
        if tag.Tag == AGENT_TAG then
            tr.Location = campPos
            agent.Destination = BEACONS[2].Pos
            agent.ReachedDestination = 0
        end
    end)
    currentTargetIndex = 2
    lastEventText = "【代理位置已重置】AI 代理已传回大本营营地，重设目标为 [2] 峡谷远端"
    lastEventTimer = 3.5
    print("[NAVMESH-SHOWCASE] 代理位置已重置回大本营")
end

-- 主逻辑帧系统
ecs:AddSystem("NavMeshShowcaseSystem", "Update", function(dt: number)
    -- 1. 关卡范围守卫检查
    if inScope == nil then
        if SceneScope and type(SceneScope.owns) == "function" then
            inScope = SceneScope.owns(AGENT_TAG) or SceneScope.owns(CAMERA_TAG) or SceneScope.owns(BARRICADE_TAG)
        else
            local found = false
            local q = ecs:Query({ "TagComponent" })
            q:Each(function(_e, tag)
                if tag.Tag == AGENT_TAG or tag.Tag == CAMERA_TAG or tag.Tag == BARRICADE_TAG then
                    found = true
                end
            end)
            inScope = found
        end
    end
    if not inScope then
        return
    end

    -- 2. 查找并绑定关键实体
    if not agentEntity or not barricadeEntity then
        local q = ecs:Query({ "TagComponent", "TransformComponent" })
        q:Each(function(e, tag, _tr)
            if tag.Tag == AGENT_TAG then
                agentEntity = e
                agentId = e:GetID()
            elseif tag.Tag == BARRICADE_TAG then
                barricadeEntity = e
                barricadeId = e:GetID()
            end
        end)

        if not agentEntity then
            return
        end

        if not initialized then
            initialized = true
            print("================================================================================")
            print("  WorldEngine 3D NavMesh & AI Navigation 综合内容展示系统已启动")
            print("  ------------------------------------------------------------------------------")
            print("  快捷键操作指南:")
            print("    [1] 键 -> 前往高台顶峰 (Plateau Hilltop) — 演示 25° 缓坡攀爬与高差寻路")
            print("    [2] 键 -> 前往峡谷远端 (Valley Exit)     — 演示直通峡谷与路障绕行")
            print("    [3] 键 -> 前往碎石前哨 (Rocky Outpost)   — 演示东侧阶梯台地巡航")
            print("    [4] 键 -> 前往大本营 (Base Camp)         — 演示返回平原大本营")
            print("    [Space] 或 [O] 键 -> 动态切换峡谷升降路障 (升起阻断 / 降下畅通)")
            print("    [P] 键            -> 开启 / 关闭四大地标多点循环巡逻模式")
            print("================================================================================")

            -- 初始设置路障状态与首发目标
            SetBarricade(true)
            SetTarget(2)
        end
    end

    -- 3. 倒计时器递减
    if rerouteTimer > 0.0 then
        rerouteTimer = math.max(0.0, rerouteTimer - dt)
    end
    if lastEventTimer > 0.0 then
        lastEventTimer = math.max(0.0, lastEventTimer - dt)
    end

    -- 4. 键盘交互响应
    -- [1] 键
    local k1 = Input.IsKeyPressed and (Input.IsKeyPressed(49) or Input.IsKeyPressed("1")) or false
    if k1 and not lastKey1 then
        patrolActive = false
        SetTarget(1)
    end
    lastKey1 = k1

    -- [2] 键
    local k2 = Input.IsKeyPressed and (Input.IsKeyPressed(50) or Input.IsKeyPressed("2")) or false
    if k2 and not lastKey2 then
        patrolActive = false
        SetTarget(2)
    end
    lastKey2 = k2

    -- [3] 键
    local k3 = Input.IsKeyPressed and (Input.IsKeyPressed(51) or Input.IsKeyPressed("3")) or false
    if k3 and not lastKey3 then
        patrolActive = false
        SetTarget(3)
    end
    lastKey3 = k3

    -- [4] 键
    local k4 = Input.IsKeyPressed and (Input.IsKeyPressed(52) or Input.IsKeyPressed("4")) or false
    if k4 and not lastKey4 then
        patrolActive = false
        SetTarget(4)
    end
    lastKey4 = k4

    -- [Space] 或 [O] 键切换路障
    local kSpace = Input.IsKeyPressed and (Input.IsKeyPressed(32) or Input.IsKeyPressed("Space")) or false
    local kO = Input.IsKeyPressed and (Input.IsKeyPressed(79) or Input.IsKeyPressed("O")) or false
    local barPressed = kSpace or kO
    if barPressed and not (lastKeySpace or lastKeyO) then
        SetBarricade(not barricadeClosed)
    end
    lastKeySpace = kSpace
    lastKeyO = kO

    -- [P] 键切换巡逻
    local kP = Input.IsKeyPressed and (Input.IsKeyPressed(80) or Input.IsKeyPressed("P")) or false
    if kP and not lastKeyP then
        patrolActive = not patrolActive
        if patrolActive then
            lastEventText = "【循环巡逻已开启】AI 代理将沿 [大本营 -> 高台 -> 峡谷出口 -> 碎石前哨] 往复循环"
            print("[NAVMESH-SHOWCASE] 循环巡逻模式已开启")
        else
            lastEventText = "【循环巡逻已关闭】维持当前目标点"
            print("[NAVMESH-SHOWCASE] 循环巡逻模式已关闭")
        end
        lastEventTimer = 3.5
    end
    lastKeyP = kP

    -- [C] 键切换蹲伏/站立态 (动态改变有效身高以穿越限高矮洞)
    local kC = Input.IsKeyPressed and (Input.IsKeyPressed(67) or Input.IsKeyPressed("C")) or false
    if kC and not lastKeyC then
        local q = ecs:Query({ "TagComponent", "NavMeshAgentComponent" })
        q:Each(function(e, tag, agent)
            if tag.Tag == AGENT_TAG then
                agent.Crouching = (agent.Crouching == 0) and 1 or 0
                if agent.Crouching == 1 then
                    lastEventText = "【高角色进入蹲伏潜行】有效高度 0.9m <= 1.3m 净空，可直接钻入半高石拱洞走捷径！"
                    print("[NAVMESH-SHOWCASE] >>> Mannequin Agent 切换为蹲伏态 (0.9m)，允许穿行 1.3m 限高石拱洞")
                else
                    lastEventText = "【高角色恢复站立姿态】站立高度 1.8m > 1.3m 净空，无法通行半高石拱洞，自动绕行主路！"
                    print("[NAVMESH-SHOWCASE] >>> Mannequin Agent 切换为站立态 (1.8m)，限高阻断，重新规划大路")
                end
                lastEventTimer = 4.0
            end
        end)
    end
    lastKeyC = kC

    -- 5. 寻路代理状态推进与朝向旋转
    local qAgent = ecs:Query({ "TagComponent", "TransformComponent", "NavMeshAgentComponent" })
    qAgent:Each(function(e, tag, tr, agent)
        if tag.Tag == AGENT_TAG then
            local targetPos = BEACONS[currentTargetIndex].Pos
            local dx = targetPos.x - tr.Location.x
            local dz = targetPos.z - tr.Location.z
            local dist = math.sqrt(dx * dx + dz * dz)

            -- 朝向平滑转向 (面对前进方向)
            if dist > 0.35 then
                local targetYawDeg = math.deg(math.atan2(dx, dz))
                setRotation(tr, 0.0, targetYawDeg, 0.0)
            end

            -- 判断是否到达
            local arrived = (agent.ReachedDestination == 1) or (dist <= agent.StoppingDistance + 0.15)
            if arrived then
                agentStateText = "Arrived (已到达)"
                if patrolActive then
                    patrolTimer = patrolTimer + dt
                    if patrolTimer >= 1.2 then
                        patrolTimer = 0.0
                        local nextIdx = (currentTargetIndex % #BEACONS) + 1
                        SetTarget(nextIdx)
                    end
                end
            elseif rerouteTimer > 0.0 then
                agentStateText = "Dynamic Rerouting (动态重规划绕行中)"
            else
                agentStateText = "Navigating (导航寻路中)"
            end
        end
    end)
end)

-- 6. 交互式 WUI HUD 实时卡片渲染
ui.onDraw(function()
    if not inScope or not initialized then
        return
    end

    -- 获取当前代理状态数据
    local currentPos = vec3.new(32.0, 0.0, 12.0)
    local targetPos = BEACONS[currentTargetIndex].Pos
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

    -- 绘制主面板
    ui.panel(24, 24, 400, 492, "3D NavMesh & AI Navigation Showcase")

    -- 状态标题与指标
    ui.text(40, 58, "状态       " .. agentStateText, 15)
    ui.text(40, 80, string.format("代理坐标   (%.1f, %.1f, %.1f)", currentPos.x, currentPos.y, currentPos.z), 13)
    ui.text(40, 100, string.format("当前目标   [%d] %s", currentTargetIndex, BEACONS[currentTargetIndex].Name), 13)
    ui.text(40, 120, string.format("剩余距离   %.2f m    巡航速度: %.1f m/s", distRemaining, speed), 13)

    -- 路障与巡逻状态
    local barricadeStatusStr = barricadeClosed and "【已升起阻断 CLOSED】(峡谷封死，代理绕行高台)" or "【已降下通行 OPEN】(峡谷畅通，直达捷径)"
    ui.text(40, 142, "峡谷路障   " .. barricadeStatusStr, 13)

    local patrolStr = patrolActive and string.format("【开启】(自动推进至目标 %d)", currentTargetIndex) or "【关闭】(单点导航)"
    ui.text(40, 164, "循环巡逻   " .. patrolStr, 13)

    -- 提示与快捷操作指南
    ui.text(40, 190, "快捷按键: [1..4] 目标点   [Space / O] 升降路障   [P] 巡逻", 12)

    -- 交互式可点击操作按钮
    if ui.button("btn_target_1", 40, 212, 85, 26, "[1] 高台") then
        patrolActive = false
        SetTarget(1)
    end
    if ui.button("btn_target_2", 130, 212, 85, 26, "[2] 峡谷") then
        patrolActive = false
        SetTarget(2)
    end
    if ui.button("btn_target_3", 220, 212, 85, 26, "[3] 前哨") then
        patrolActive = false
        SetTarget(3)
    end
    if ui.button("btn_target_4", 310, 212, 85, 26, "[4] 营地") then
        patrolActive = false
        SetTarget(4)
    end

    local barBtnText = barricadeClosed and "降下峡谷路障 (切换为畅通通行 OPEN)" or "升起峡谷路障 (切换为阻断雕刻 CLOSED)"
    if ui.button("btn_toggle_barricade", 40, 246, 355, 28, barBtnText) then
        SetBarricade(not barricadeClosed)
    end

    local patrolBtnText = patrolActive and "停止四点循环巡逻 (切换为单点停留)" or "启动四点循环巡逻 (自动往复漫游)"
    if ui.button("btn_toggle_patrol", 40, 282, 355, 28, patrolBtnText) then
        patrolActive = not patrolActive
        lastEventText = patrolActive and "【已启动循环巡逻】" or "【已停止循环巡逻】"
        lastEventTimer = 3.0
    end

    if ui.button("btn_reset_agent", 40, 318, 355, 28, "重置 AI 代理位置 (返回大本营营地)") then
        patrolActive = false
        ResetAgentToBaseCamp()
    end

    -- 动态操作提示文字
    ui.text(40, 356, "最近操作: " .. lastEventText, 11)

    -- 底部署名与原理摘要
    ui.text(40, 384, "3D 寻路与避障机制技术解析:", 12)
    ui.text(40, 404, "- 算法核心: 3D A* 凸多边形走廊搜索 + Funnel 漏斗最短折线收紧", 10)
    ui.text(40, 420, "- 斜率剔除: >45° 陡壁绝壁自动剔除，25° 平缓坡道无缝登高", 10)
    ui.text(40, 436, "- 动态雕刻: NavMeshObstacleComponent 实时打洞剔除重叠多边形", 10)
    ui.text(40, 452, "- 实时重规划: 峡谷受阻毫秒级感知，沿西侧高台缓坡智能绕行", 10)
end)

-- 7. 场景卸载清理系统
ecs:AddTeardownSystem("NavMeshShowcaseTeardown", function()
    initialized = false
    inScope = nil
    agentEntity = nil
    agentId = nil
    barricadeEntity = nil
    barricadeId = nil
    patrolActive = false
    print("[NAVMESH-SHOWCASE] 演示系统已安全执行收尾清理 (Teardown)")
end)

print("[PureECS] NavMeshShowcaseSystem 与 TeardownSystem 已注册至场景生命周期")
