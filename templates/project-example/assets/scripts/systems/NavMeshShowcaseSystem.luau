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
local GOBLIN_TAG = "Goblin Scout (Short: 0.9m)"
local BARRICADE_TAG = "Valley Barricade"
local CAMERA_TAG = "NavMesh Showcase Camera"
local TERRAIN_TAG = "Showcase Terrain"

local selectedAgentTag = AGENT_TAG

local inScope = false
if SceneScope and type(SceneScope.owns) == "function" then
    inScope = SceneScope.owns(AGENT_TAG) or SceneScope.owns(CAMERA_TAG) or SceneScope.owns(BARRICADE_TAG)
else
    local q = ecs:Query({ "TagComponent" })
    if q and type(q.Each) == "function" then
        q:Each(function(_e, tag)
            if tag.Tag == AGENT_TAG or tag.Tag == CAMERA_TAG or tag.Tag == BARRICADE_TAG then
                inScope = true
            end
        end)
    end
end

if not inScope then
    return
end

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
    {
        Name = "Arch Tunnel (低矮石拱洞 净空:1.3m)",
        Pos = vec3.new(27.0, 0.0, 48.0),
        Description = "低矮石拱通道，过梁净空 1.3m。矮小角色(0.9m)或蹲伏角色(0.9m)可通行；高角色站立(1.8m)阻断",
    },
}

-- 运行时状态
local initialized = false
local inScope = nil

local agentEntity = nil
local agentId = nil
local goblinEntity = nil
local barricadeEntity = nil
local barricadeId = nil
local cameraEntity = nil

-- 相机视角控制: 默认全景鸟瞰视角 (俯瞰全图)，支持切换为第三人称跟随视角
local followCamera = false
local lastKeyV = false
local lastKey5 = false

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

    local q = ecs:Query({ "TagComponent", "NavMeshAgentComponent" })
    q:Each(function(e, tag, agent)
        if tag.Tag == selectedAgentTag or tag.Tag == AGENT_TAG then
            agent.Destination = target.Pos
            agent.ReachedDestination = 0
            agent.AutoRepath = 1
        end
    end)

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
            elseif tag.Tag == GOBLIN_TAG then
                goblinEntity = e
            elseif tag.Tag == BARRICADE_TAG then
                barricadeEntity = e
                barricadeId = e:GetID()
            elseif tag.Tag == CAMERA_TAG then
                cameraEntity = e
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

            -- 示例展示包含交互式 WUI 按钮，通过游戏逻辑显式调出鼠标光标
            if Input and type(Input.SetCursorVisible) == "function" then
                Input.SetCursorVisible(true)
            end
            if Input and type(Input.SetCursorLocked) == "function" then
                Input.SetCursorLocked(false)
            end
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

    -- [5] 键: 穿行低矮石拱洞对比测试
    local k5 = Input.IsKeyPressed and (Input.IsKeyPressed(53) or Input.IsKeyPressed("5")) or false
    if k5 and not lastKey5 then
        patrolActive = false
        SetTarget(5)
    end
    lastKey5 = k5

    -- [V] 键: 切换相机视角 (全景鸟瞰 / 第三人称跟随)
    local kV = Input.IsKeyPressed and (Input.IsKeyPressed(86) or Input.IsKeyPressed("V")) or false
    if kV and not lastKeyV then
        followCamera = not followCamera
        lastEventText = followCamera and "【相机视角】已切换为【第三人称跟随视角】" or "【相机视角】已切换为【全景宏观鸟瞰视角】"
        lastEventTimer = 3.5
    end
    lastKeyV = kV

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
            if tag.Tag == selectedAgentTag then
                agent.Crouching = (agent.Crouching == 0) and 1 or 0
                local effH = (agent.Crouching == 1) and (agent.CrouchHeight or 0.9) or (agent.AgentHeight or 1.8)
                if agent.Crouching == 1 then
                    lastEventText = string.format("【%s 蹲伏潜行】有效高度 %.2fm <= 1.3m 净空，可通过半高石拱洞！", tag.Tag, effH)
                else
                    lastEventText = string.format("【%s 站立姿态】有效高度 %.2fm > 1.3m 净空，受限高阻断！", tag.Tag, effH)
                end
                lastEventTimer = 4.0
            end
        end)
    end
    lastKeyC = kC

    -- 5. 寻路代理状态推进与朝向旋转 (支持双角色多体型)
    local qAgent = ecs:Query({ "TagComponent", "TransformComponent", "NavMeshAgentComponent" })
    qAgent:Each(function(e, tag, tr, agent)
        if tag.Tag == AGENT_TAG or tag.Tag == GOBLIN_TAG then
            local targetPos = BEACONS[currentTargetIndex].Pos
            local dx = targetPos.x - tr.Location.x
            local dz = targetPos.z - tr.Location.z
            local dist = math.sqrt(dx * dx + dz * dz)

            -- 面向移动方向平滑旋转朝向
            if dist > 0.35 then
                local targetYawDeg = math.deg(math.atan2(dx, dz))
                setRotation(tr, 0.0, targetYawDeg, 0.0)
            end

            -- 仅对当前受控代理更新状态文本与巡逻推进
            if tag.Tag == selectedAgentTag then
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
        end
    end)

    -- 6. 实时驱动视口主相机位置与朝向，保证 Play 模式下画面完美呈现
    if cameraEntity then
        local qCam = ecs:Query({ "TagComponent", "TransformComponent", "CameraComponent" })
        qCam:Each(function(e, tag, tr, cam)
            if tag.Tag == CAMERA_TAG then
                cam.Primary = true
                if followCamera then
                    -- 第三人称追随当前代理视角 (位于代理后上方 4m，紧跟运动轨迹)
                    local curPos = vec3.new(32.0, 0.0, 12.0)
                    local qAg = ecs:Query({ "TagComponent", "TransformComponent" })
                    qAg:Each(function(ae, atag, atr)
                        if atag.Tag == selectedAgentTag then
                            curPos = atr.Location
                        end
                    end)
                    tr.Location = curPos + vec3.new(0.0, 4.0, -7.0)
                    setRotation(tr, -22.0, 180.0, 0.0)
                else
                    -- 全景宏观鸟瞰视角: 位于场地南侧上方 (32, 22, -6)，俯瞰大本营、石拱矮洞、峡谷与高台
                    tr.Location = vec3.new(32.0, 22.0, -6.0)
                    setRotation(tr, -25.35, 180.0, 0.0)
                end
            end
        end)
    end
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
        if tag.Tag == selectedAgentTag then
            currentPos = tr.Location
            speed = agent.Speed
            local dx = targetPos.x - currentPos.x
            local dy = targetPos.y - currentPos.y
            local dz = targetPos.z - currentPos.z
            distRemaining = math.sqrt(dx * dx + dy * dy + dz * dz)
        end
    end)

    -- 绘制主面板
    ui.panel(24, 24, 400, 582, "3D NavMesh & AI Navigation Showcase")

    -- 状态标题与指标
    ui.text(40, 56, "状态       " .. agentStateText, 14)
    local curAgentH = 1.8
    local curCrouch = 0
    local qAgentInfo = ecs:Query({ "TagComponent", "NavMeshAgentComponent" })
    qAgentInfo:Each(function(e, tag, agent)
        if tag.Tag == selectedAgentTag then
            curCrouch = agent.Crouching
            curAgentH = (curCrouch == 1) and (agent.CrouchHeight or 0.9) or (agent.AgentHeight or 1.8)
        end
    end)
    ui.text(40, 76, string.format("当前代理   [%s] 有效身高: %.2fm", (selectedAgentTag == AGENT_TAG and "人偶 1.8m" or "地精 0.9m"), curAgentH), 12)
    ui.text(40, 94, string.format("当前目标   [%d] %s", currentTargetIndex, BEACONS[currentTargetIndex].Name), 12)
    ui.text(40, 112, string.format("剩余距离   %.2f m    巡航速度: %.1f m/s", distRemaining, speed), 12)

    -- 路障与巡逻状态
    local barricadeStatusStr = barricadeClosed and "【CLOSED 阻断】代理自动绕行高台" or "【OPEN 畅通】峡谷捷径直达"
    ui.text(40, 130, "峡谷路障   " .. barricadeStatusStr, 12)

    local patrolStr = patrolActive and string.format("【开启】(目标 %d)", currentTargetIndex) or "【关闭】"
    ui.text(40, 148, "循环巡逻   " .. patrolStr, 12)

    -- 快捷按键提示
    ui.text(40, 168, "按键: [1..5] 目标   [C] 蹲伏   [V] 视角   [Space] 路障   [P] 巡逻", 11)

    -- 相机视角切换按钮
    local camBtnText = followCamera
        and "当前视角: 【第三人称跟随】 -> 点击切换为 【全景鸟瞰视角】 (按 V 键)"
        or "当前视角: 【全景宏观鸟瞰】 -> 点击切换为 【第三人称跟随】 (按 V 键)"
    if ui.button("btn_toggle_cam", 40, 186, 355, 24, camBtnText) then
        followCamera = not followCamera
        lastEventText = followCamera and "【相机切换】已切换为【第三人称跟随视角】" or "【相机切换】已切换为【全景宏观鸟瞰视角】"
        lastEventTimer = 3.0
    end

    -- 地标按钮 (四点 + 石拱洞)
    if ui.button("btn_target_1", 40, 214, 68, 24, "[1] 高台") then
        patrolActive = false
        SetTarget(1)
    end
    if ui.button("btn_target_2", 112, 214, 68, 24, "[2] 峡谷") then
        patrolActive = false
        SetTarget(2)
    end
    if ui.button("btn_target_3", 184, 214, 68, 24, "[3] 前哨") then
        patrolActive = false
        SetTarget(3)
    end
    if ui.button("btn_target_4", 256, 214, 68, 24, "[4] 营地") then
        patrolActive = false
        SetTarget(4)
    end
    if ui.button("btn_target_5", 328, 214, 68, 24, "[5] 矮洞") then
        patrolActive = false
        SetTarget(5)
    end

    -- 多体型角色与低矮限高通道核心测试按钮
    local toggleAgentBtnText = (selectedAgentTag == AGENT_TAG)
        and "切换受控代理: [当前: 标准人偶 1.8m] -> 点击切换为 地精 0.9m"
        or "切换受控代理: [当前: 矮小地精 0.9m] -> 点击切换为 人偶 1.8m"
    if ui.button("btn_toggle_agent", 40, 242, 355, 26, toggleAgentBtnText) then
        selectedAgentTag = (selectedAgentTag == AGENT_TAG) and GOBLIN_TAG or AGENT_TAG
        lastEventText = string.format("【切换代理】当前受控: %s", selectedAgentTag)
        lastEventTimer = 3.5
    end

    local crouchBtnText = (curCrouch == 1)
        and "恢复站立姿态 (按 C 键) -> 站立有效高度 1.8m (限高阻断)"
        or "进入蹲伏姿态 (按 C 键) -> 蹲伏有效高度 0.9m (钻入矮洞)"
    if ui.button("btn_toggle_crouch", 40, 272, 355, 26, crouchBtnText) then
        local q = ecs:Query({ "TagComponent", "NavMeshAgentComponent" })
        q:Each(function(e, tag, agent)
            if tag.Tag == selectedAgentTag then
                agent.Crouching = (agent.Crouching == 0) and 1 or 0
                local effH = (agent.Crouching == 1) and (agent.CrouchHeight or 0.9) or (agent.AgentHeight or 1.8)
                lastEventText = string.format("【姿态切换】%s 有效高度: %.2fm", tag.Tag, effH)
                lastEventTimer = 3.5
            end
        end)
    end

    local barBtnText = barricadeClosed and "降下峡谷路障 (切换为畅通通行 OPEN)" or "升起峡谷路障 (切换为阻断雕刻 CLOSED)"
    if ui.button("btn_toggle_barricade", 40, 302, 355, 26, barBtnText) then
        SetBarricade(not barricadeClosed)
    end

    local patrolBtnText = patrolActive and "停止四点循环巡逻 (切换为单点停歇)" or "启动四点循环巡逻 (自动往复漫游)"
    if ui.button("btn_toggle_patrol", 40, 332, 355, 26, patrolBtnText) then
        patrolActive = not patrolActive
        lastEventText = patrolActive and "【已启动循环巡逻】" or "【已停止循环巡逻】"
        lastEventTimer = 3.0
    end

    if ui.button("btn_reset_agent", 40, 362, 355, 26, "重置 AI 代理位置 (返回大本营营地)") then
        patrolActive = false
        ResetAgentToBaseCamp()
    end

    -- 动态操作提示文字
    ui.text(40, 396, "最近操作: " .. lastEventText, 11)

    -- 底部签名与原理摘要
    ui.text(40, 420, "3D 寻路与限高避障机制技术解析:", 12)
    ui.text(40, 438, "- 连续物理净空: NavMeshAreaModifierComponent ClearanceHeight 精准过滤", 10)
    ui.text(40, 454, "- 多体型自适应: 0.9m 地精天生穿过 1.3m 矮洞；1.8m 人偶蹲伏后方可通行", 10)
    ui.text(40, 470, "- 动态雕刻: NavMeshObstacleComponent 实时打洞剔除重叠多边形", 10)
    ui.text(40, 486, "- 实时重规划: 遇阻毫秒级感知，沿西侧高台缓坡智能绕行", 10)end)

-- 7. 场景卸载清理系统
ecs:AddTeardownSystem("NavMeshShowcaseTeardown", function()
    initialized = false
    inScope = nil
    agentEntity = nil
    agentId = nil
    barricadeEntity = nil
    barricadeId = nil
    cameraEntity = nil
    followCamera = false
    patrolActive = false
    print("[NAVMESH-SHOWCASE] 演示系统已安全执行收尾清理 (Teardown)")
end)

print("[PureECS] NavMeshShowcaseSystem 与 TeardownSystem 已注册至场景生命周期")
