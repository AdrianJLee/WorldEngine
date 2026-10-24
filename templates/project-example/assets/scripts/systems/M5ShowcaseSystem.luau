-- =============================================================================
-- Milestone M5 综合成果实机展示系统 (M5 Showcase System)
-- =============================================================================
-- 演示 M5 交付的核心系统与通用工具集成:
--   1. Particle Core (通用粒子系统):
--      - 2D/3D 粒子池模拟、几何发射器 (Point / Cone)、生命周期颜色与大小衰减
--      - Luau Particle.* 服务接口: Emit / Play / Stop / SetEmissionRate / SetLockToXY
--   2. Bezier Curve Easing (贝塞尔曲线与缓动求值):
--      - 三次贝塞尔分析求值，驱动实体弹跳与形变弹性动画
--   3. Selection Filter & Undo/Redo Tooling (选择过滤器与撤销历史):
--      - 视口图层归属与选择遮罩
-- =============================================================================

local initialized = false
local fireEmitterEntity = nil
local fireEmitterId = nil

local sparkEmitterEntity = nil
local sparkEmitterId = nil

local bouncerEntity = nil
local bouncerId = nil

-- 贝塞尔求值参数
local curveTime = 0.0
local curveDuration = 1.6

-- 按键防抖
local lastSpaceKey = false
local lastKey1 = false
local lastKey2 = false
local lastKey3 = false

local is2DLocked = false
local currentRate = 80.0
local logTimer = 0.0

-- 三次贝塞尔 Bounce 缓动算法 (同 CubicBezierCurve::Bounce())
local function EvaluateBounceEase(t: number): number
    t = math.clamp(t, 0.0, 1.0)
    if t < (1.0 / 2.75) then
        return 7.5625 * t * t
    elseif t < (2.0 / 2.75) then
        t = t - (1.5 / 2.75)
        return 7.5625 * t * t + 0.75
    elseif t < (2.5 / 2.75) then
        t = t - (2.25 / 2.75)
        return 7.5625 * t * t + 0.9375
    else
        t = t - (2.625 / 2.75)
        return 7.5625 * t * t + 0.984375
    end
end

ecs:AddSystem("M5ShowcaseSystem", "Update", function(dt: number)
    -- 若尚未绑定展示实体，扫描当前场景
    if not fireEmitterEntity and not sparkEmitterEntity and not bouncerEntity then
        local q = ecs:Query({ "TagComponent", "TransformComponent" })
        q:Each(function(e, tag, tr)
            if tag.Tag == "Fire Particle Emitter" then
                fireEmitterEntity = e
                fireEmitterId = e:GetID()
            elseif tag.Tag == "Spark Fountain Emitter" then
                sparkEmitterEntity = e
                sparkEmitterId = e:GetID()
            elseif tag.Tag == "Curve Animated Bouncer" then
                bouncerEntity = e
                bouncerId = e:GetID()
            end
        end)

        -- 若当前场景非 M5Showcase（无相关实体），静默早退
        if not fireEmitterEntity and not sparkEmitterEntity and not bouncerEntity then
            return
        end

        if not initialized then
            initialized = true
            print("[M5-SHOWCASE] ════════════════════════════════════════════════════════════════")
            print("[M5-SHOWCASE] Milestone M5 粒子系统与编辑器通用工具套件实机演示启动")
            if fireEmitterId then print(string.format("[M5-SHOWCASE] 发现火焰粒子发射器 [ID: %d]", fireEmitterId)) end
            if sparkEmitterId then print(string.format("[M5-SHOWCASE] 发现火花喷泉发射器 [ID: %d]", sparkEmitterId)) end
            if bouncerId then print(string.format("[M5-SHOWCASE] 发现曲线动画弹跳体 [ID: %d]", bouncerId)) end
            print("[M5-SHOWCASE] 交互控制说明:")
            print("[M5-SHOWCASE]   [Space 空格] -> 瞬发粒子爆炸喷射 (Burst 80 颗粒子)")
            print("[M5-SHOWCASE]   [1 键]       -> 加快粒子发射速率 (+30/s)")
            print("[M5-SHOWCASE]   [2 键]       -> 降低粒子发射速率 (-30/s)")
            print("[M5-SHOWCASE]   [3 键]       -> 切换 2D XY平面锁定 / 3D 自由空间模拟")
            print("[M5-SHOWCASE]   [菜单栏 Window] -> 可打开 Curve Editor (曲线编辑器) 与 Selection Filter (选择过滤器)")
            print("[M5-SHOWCASE] ════════════════════════════════════════════════════════════════")
        end
    end

    logTimer = logTimer + dt
    curveTime = curveTime + dt
    if curveTime > curveDuration then
        curveTime = curveTime - curveDuration
    end

    -- 2. 交互键盘响应
    local spaceDown = false
    if Input.IsKeyPressed then
        spaceDown = Input.IsKeyPressed(32) or Input.IsKeyPressed("Space")
    end
    if spaceDown and not lastSpaceKey then
        if sparkEmitterId then
            Particle.Emit(sparkEmitterId, 80)
            print("[M5-SHOWCASE] >>> [用户按下空格] 触发火花发射器 Burst 瞬发喷射 80 颗粒子!")
        end
    end
    lastSpaceKey = spaceDown

    local key1Down = false
    if Input.IsKeyPressed then
        key1Down = Input.IsKeyPressed(49) or Input.IsKeyPressed("1")
    end
    if key1Down and not lastKey1 then
        currentRate = math.min(currentRate + 30.0, 500.0)
        if fireEmitterId then
            Particle.SetEmissionRate(fireEmitterId, currentRate)
            print(string.format("[M5-SHOWCASE] >>> [用户按下 1 键] 火焰发射速率提升至: %.1f 颗/秒", currentRate))
        end
    end
    lastKey1 = key1Down

    local key2Down = false
    if Input.IsKeyPressed then
        key2Down = Input.IsKeyPressed(50) or Input.IsKeyPressed("2")
    end
    if key2Down and not lastKey2 then
        currentRate = math.max(currentRate - 30.0, 10.0)
        if fireEmitterId then
            Particle.SetEmissionRate(fireEmitterId, currentRate)
            print(string.format("[M5-SHOWCASE] >>> [用户按下 2 键] 火焰发射速率降低至: %.1f 颗/秒", currentRate))
        end
    end
    lastKey2 = key2Down

    local key3Down = false
    if Input.IsKeyPressed then
        key3Down = Input.IsKeyPressed(51) or Input.IsKeyPressed("3")
    end
    if key3Down and not lastKey3 then
        is2DLocked = not is2DLocked
        if fireEmitterId then
            Particle.SetLockToXY(fireEmitterId, is2DLocked)
        end
        if sparkEmitterId then
            Particle.SetLockToXY(sparkEmitterId, is2DLocked)
        end
        print(string.format("[M5-SHOWCASE] >>> [用户按下 3 键] 粒子空间锁定模式切换为: %s",
            is2DLocked and "2D (XY 平面锁定, Z=0)" or "3D (全自由空间)"))
    end
    lastKey3 = key3Down

    -- 3. 驱动贝塞尔曲线动画 (Cubic Bezier Easing)
    if bouncerEntity then
        local tr = bouncerEntity:GetComponent("TransformComponent")
        if tr then
            local normT = curveTime / curveDuration
            local bounceVal = EvaluateBounceEase(normT)
            local loc = tr.Location
            loc.y = 0.5 + bounceVal * 2.5
            tr.Location = loc

            -- 触底弹性挤压 (Squash & Stretch)
            local sc = tr.Scale
            if bounceVal < 0.1 then
                sc.x = 1.3
                sc.y = 0.7
                sc.z = 1.3
            else
                sc.x = 0.9
                sc.y = 1.15
                sc.z = 0.9
            end
            tr.Scale = sc
        end
    end

    -- 4. 周期性现场状态快照
    if logTimer >= 2.5 then
        logTimer = 0.0
        local fireCount = fireEmitterId and Particle.GetActiveCount(fireEmitterId) or 0
        local sparkCount = sparkEmitterId and Particle.GetActiveCount(sparkEmitterId) or 0
        print(string.format("[M5-HUD] 活粒子总数: %d (火焰: %d, 火花: %d) | 发射速率: %.0f/s | 空间模式: %s",
            fireCount + sparkCount, fireCount, sparkCount, currentRate,
            is2DLocked and "2D 平面" or "3D 空间"))
    end
end)

ecs:AddTeardownSystem("M5ShowcaseTeardown", function()
    fireEmitterEntity = nil
    fireEmitterId = nil
    sparkEmitterEntity = nil
    sparkEmitterId = nil
    bouncerEntity = nil
    bouncerId = nil
    initialized = false
end)
