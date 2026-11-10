-- =============================================================================
-- M2 镜头与后处理综合演示系统 (CameraPostProcessDemoSystem.luau)
-- =============================================================================
-- 本系统演示 WorldEngine M2 核心系统在示例工程中的运行:
--   1. VirtualCameraSystem: Cinemachine 临界阻尼平滑跟踪 (SmoothDamp)
--   2. 屏幕死区 (DeadZone) 与 速度前瞻 (Lookahead)
--   3. Trauma 震屏外伤注入与非线性衰减 (Camera.Shake)
--   4. 全屏后处理管线: 受击高亮闪白 (PostProcess.FlashWhite)
--   5. 发光辉光 (Bloom)、轮廓描边 (Outline) 与电影级暗角 (Vignette)
-- =============================================================================

-- 关卡归属守卫: 本演示只在含 "Hero Character" 的关卡里生效。
-- 否则会在别的关卡里抢 空格(震屏)/F(闪白)/A、D 键, 干扰该关卡自己的操作。
local SceneScope = nil
local okScope, scopeLib = pcall(function() return ecs:RequireLib("scene_scope") end)
if okScope and type(scopeLib) == "table" then
    SceneScope = scopeLib
end

local initialized = false
local inScope: boolean? = nil
local heroId = nil
local timer = 0.0
local logTimer = 0.0
local lastSpace = false
local lastF = false
local lastLmb = false

ecs:AddSystem("CameraPostProcessDemoSystem", "Update", function(dt: number)
    if inScope == nil then
        if SceneScope and type(SceneScope.owns) == "function" then
            inScope = SceneScope.owns("Hero Character")
        else
            local found = false
            local q = ecs:Query({ "TagComponent" })
            q:Each(function(_e, tag)
                if tag.Tag == "Hero Character" then found = true end
            end)
            inScope = found
        end
    end
    if not inScope then
        return
    end
    timer = timer + dt
    logTimer = logTimer + dt

    -- 1. 初次启动：查找主角 ID 与绑定虚拟相机跟随目标
    if not initialized then
        initialized = true
        print("[M2-DEMO] ═══════════════════════════════════════════════════════")
        print("[M2-DEMO] M2 镜头与后处理演示系统已成功启动")

        local heroQuery = ecs:Query({ "TagComponent", "TransformComponent" })
        heroQuery:Each(function(e, tag, tr)
            if tag.Tag == "Hero Character" then
                heroId = e:GetID()
                print(string.format("[M2-DEMO] 找到主角实体 [ID: %d], 初始坐标=(%.2f, %.2f, %.2f)",
                    heroId, tr.Location.x, tr.Location.y, tr.Location.z))
            end
        end)

        local vcamQuery = ecs:Query({ "TagComponent", "VirtualCameraComponent" })
        vcamQuery:Each(function(e, tag, vcam)
            if tag.Tag == "VirtualFollowCamera" then
                if heroId then
                    Camera.SetTarget(heroId)
                    print(string.format("[M2-DEMO] 虚拟相机已绑定目标主角 [ID: %d]", heroId))
                end
                print(string.format("[M2-DEMO] 相机参数: 阻尼=(%.2f, %.2f, %.2f) | 死区=(%.2f, %.2f) | 前瞻=%.2fs",
                    vcam.Damping.x, vcam.Damping.y, vcam.Damping.z, vcam.DeadZone.x, vcam.DeadZone.y, vcam.LookaheadTime))
            end
        end)
        print("[M2-DEMO] 键盘交互操作:")
        print("[M2-DEMO]   [Space 空格键] -> 触发相机剧烈震屏外伤 (Camera.Shake 1.0)")
        print("[M2-DEMO]   [F 键]         -> 触发全屏受击高亮闪白 (PostProcess.FlashWhite 1.0)")
        print("[M2-DEMO]   [鼠标左键]     -> 触发轻度受击晃动 (Camera.Shake 0.5)")
        print("[M2-DEMO]   [A / D 键]     -> 手动操控主角移动走位 (松开后恢复自动巡逻)")
        print("[M2-DEMO] ═══════════════════════════════════════════════════════")
    end

    -- 2. 交互式键盘/鼠标按键响应 (空格键 Space -> 震屏 / F 键 -> 闪白)
    -- 空格键检测 (支持物理按键与增强输入动作 Jump/Dodge)
    local spaceDown = false
    if Input.IsKeyPressed then
        spaceDown = Input.IsKeyPressed("Space") or Input.IsKeyPressed(32)
    end
    if not spaceDown then
        spaceDown = Input.Down("Jump") or Input.Pressed("Jump") or Input.Pressed("Dodge")
    end

    if spaceDown and not lastSpace then
        Camera.Shake(1.0)
        print("[M2-DEMO] >>> [KEY: Space] 空格键按下 -> 触发相机剧烈震屏外伤! 注入 Trauma 1.0")
    end
    lastSpace = spaceDown

    -- F 键检测 (支持物理按键与增强输入动作 Interact)
    local fDown = false
    if Input.IsKeyPressed then
        fDown = Input.IsKeyPressed("F") or Input.IsKeyPressed(70)
    end
    if not fDown then
        fDown = Input.Down("Interact") or Input.Pressed("Interact")
    end

    if fDown and not lastF then
        PostProcess.FlashWhite(1.0, 3.5)
        print("[M2-DEMO] >>> [KEY: F] F 键按下 -> 触发全屏受击高亮闪白! (FlashWhite 1.0, 衰减 3.5/s)")
    end
    lastF = fDown

    -- 鼠标左键检测 (可选操作: 点击视口直接触发震动)
    local lmbDown = Input.Pressed("Fire")
    if lmbDown and not lastLmb then
        Camera.Shake(0.5)
        print("[M2-DEMO] >>> [MOUSE: LMB] 鼠标左键点击 -> 触发受击震屏 (Trauma 0.5)")
    end
    lastLmb = lmbDown

    -- 3. 角色走位控制 (支持 A/D 键手动操控，未按键时保持自动平滑巡逻)
    local manualAxis = 0.0
    if Input.IsKeyPressed then
        if Input.IsKeyPressed("A") or Input.IsKeyPressed("Left") then manualAxis = manualAxis - 1.0 end
        if Input.IsKeyPressed("D") or Input.IsKeyPressed("Right") then manualAxis = manualAxis + 1.0 end
    end
    if manualAxis == 0.0 then
        manualAxis = Input.Axis("Move")
    end

    local heroQuery = ecs:Query({ "TagComponent", "TransformComponent" })
    heroQuery:Each(function(e, tag, tr)
        if tag.Tag == "Hero Character" then
            local speed = 6.0
            if math.abs(manualAxis) > 0.05 then
                local velX = manualAxis * speed
                local newX = tr.Location.x + velX * dt
                newX = math.max(-10.0, math.min(10.0, newX))
                tr.Location = vec3.new(newX, tr.Location.y, tr.Location.z)

                if e:HasComponent("CharacterControllerComponent") then
                    local kcc = e:GetComponent("CharacterControllerComponent")
                    kcc.Velocity = vec3.new(velX, 0.0, 0.0)
                end
            else
                local newX = math.sin(timer * 0.8) * 5.0
                local velX = math.cos(timer * 0.8) * 4.0
                local newY = 1.2 + math.abs(math.sin(timer * 3.0)) * 0.4

                tr.Location = vec3.new(newX, newY, tr.Location.z)

                if e:HasComponent("CharacterControllerComponent") then
                    local kcc = e:GetComponent("CharacterControllerComponent")
                    kcc.Velocity = vec3.new(velX, 0.0, 0.0)
                end
            end
        end
    end)

    -- 4. 周期性状态回传日志 (每 4 秒输出一次当前相机位置与状态)
    if logTimer >= 4.0 then
        logTimer = 0.0
        local camPos = Camera.GetPosition()
        print(string.format("[M2-DEMO-TICK] 主相机坐标=(%.2f, %.2f, %.2f) | 运行时间=%.1fs",
            camPos.x, camPos.y, camPos.z, timer))
    end
end)

ecs:AddTeardownSystem("CameraPostProcessDemoTeardown", function()
    inScope = nil
    initialized = false
    heroId = nil
end)
