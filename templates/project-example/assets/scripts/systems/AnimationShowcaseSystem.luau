-- =============================================================================
-- WorldEngine — Animation Showcase System (Milestone 4)
-- =============================================================================
-- 用户可操作的 AnimGraph 内容示例:
--   WASD / 左摇杆 LX 移动, Shift 奔跑, Space 跳跃,
--   F 瞄准(上半身层), R 切换 Root Motion, Tab 切换跟随/自由视角.
--
-- 设计口径:
--   * 脚本只写 CharacterControllerComponent.Velocity / Grounded;
--     Speed / Direction / IsFalling 由 AnimGraphSystem 从 Velocity 推导.
--     脚本绝不写 AnimGraphComponent.CurrentStateHash.
--   * Transform.Rotation 目前没有 Luau 映射(schema kind = Quat),所以角色保持
--     绑定朝向 +Z(mannequin.gltf 即 faces +Z). W=前进, S=后退, A/D=侧移,
--     这正好展示 Locomotion 2D 混合空间(Direction = 0 / ±90 / 180).
--   * 地面高度优先取 Physics.Raycast(地形以后补上碰撞即自动生效);
--     否则按 animation_showcase.wterrain 的生成公式解析求值(与
--     Engine/generators/mannequin/generate_showcase_terrain.py 逐项一致).
--   * 找不到本关实体时静默退出,不打扰其它场景; 组件缺失只提示一次.
-- =============================================================================

local CHARACTER_TAG = "Mannequin Character"
local CAMERA_TAG = "Showcase Camera"
local SHOWCASE_TAG = "Showcase Terrain"

local WALK_SPEED = 1.6          -- m/s
local RUN_SPEED = 4.5           -- m/s
local ACCELERATION = 12.0       -- 1/s, exponential approach
local JUMP_SPEED = 5.2          -- m/s
local GRAVITY = -18.0           -- m/s^2
local GROUND_SNAP_RATE = 18.0   -- 1/s
local CAMERA_OFFSET_X = 0.0
local CAMERA_OFFSET_Y = 2.2
local CAMERA_OFFSET_Z = -4.5
local CAMERA_FOLLOW_RATE = 6.0  -- 1/s (dispatch: k ~= 6)
local CAMERA_MIN_CLEARANCE = 0.8
local MAX_STEP_UP = 0.45        -- m, higher than StepOffset blocks the move
local WORLD_MIN_X = -22.0       -- keep the character on the 48 m showcase slab/terrain
local WORLD_MAX_X = 38.0
local WORLD_MIN_Z = -22.0
local WORLD_MAX_Z = 38.0

-- animation_showcase.wterrain 的解析镜像(与 generate_showcase_terrain.py 同公式):
--   地形实体位于 (-24,0,-24), 65x65 网格 / cell 1 m / heightScale 1.
--   本地 X 30..44 smoothstep 升到 2.35 m, 44..64 微起伏, 叠加 ripple 与 basin.
local TERRAIN_ORIGIN_X = -24.0
local TERRAIN_ORIGIN_Z = -24.0

local KEY_A = 65
local KEY_D = 68
local KEY_W = 87
local KEY_S = 83
local KEY_SPACE = 32
local KEY_LEFT_SHIFT = 340
local KEY_F = 70
local KEY_R = 82
local KEY_TAB = 258

local character = nil
local characterId = nil
local cameraEntity = nil
local charTransform = nil
local charAnim = nil
local charController = nil

local bound = false
local foreignScene = false
local resolveTimer = 1.0
local warnedAnim = false
local warnedController = false
local animationServiceWarned = false
local startupPrinted = false

local speed = 0.0
local dirX = 0.0
local dirZ = 1.0
local verticalVelocity = 0.0
local grounded = true
local aim = false
local rootMotion = false
local followCamera = true
local camX = 0.0
local camY = 0.0
local camZ = 0.0
local previousJumpHeld = false
local previousAimHeld = false
local previousRootMotionHeld = false
local previousCameraHeld = false
local lastEvent = "就绪"
local lastEventTimer = 0.0

-- 屏幕控制条(鼠标可玩):与键盘输入等价,同时给自动化一个可点击的用户入口。
local demoMove = 0          -- 0 = 停止, 1 = 前进, -1 = 后退
local demoRun = false
local demoJump = false
local demoAimToggle = false
local demoRootToggle = false
local demoCameraToggle = false
local demoTeleport = nil       -- "slope" | "origin"

local function approach(current, target, rate, dt)
    return current + (target - current) * (1.0 - math.exp(-rate * dt))
end

local function keyDown(name, code)
    local ok, value = pcall(Input.IsKeyPressed, name)
    if ok and value == true then
        return true
    end
    ok, value = pcall(Input.IsKeyPressed, code)
    return ok and value == true
end

local function actionDown(name)
    if type(Input.Down) ~= "function" then
        return false
    end
    local ok, value = pcall(Input.Down, name)
    return ok and value == true
end

local function actionPressed(name)
    if type(Input.Pressed) ~= "function" then
        return false
    end
    local ok, value = pcall(Input.Pressed, name)
    return ok and value == true
end

local function axisValue(name)
    if type(Input.Axis) ~= "function" then
        return 0.0
    end
    local ok, value = pcall(Input.Axis, name)
    if ok and type(value) == "number" then
        return value
    end
    return 0.0
end

local function firstNonZero(...)
    local values = { ... }
    for i = 1, #values do
        if math.abs(values[i]) > 0.01 then
            return values[i]
        end
    end
    return 0.0
end

local function smoothstep(a, b, x)
    local t = math.max(0.0, math.min(1.0, (x - a) / (b - a)))
    return t * t * (3.0 - 2.0 * t)
end

local function profileGround(worldX, worldZ)
    local x = worldX - TERRAIN_ORIGIN_X
    local z = worldZ - TERRAIN_ORIGIN_Z
    local ramp = smoothstep(30.0, 44.0, x) * 2.35
    local plateauRoll = smoothstep(44.0, 64.0, x) * (0.10 * math.sin(z * 0.20))
    local dx = x - 12.0
    local dz = z - 40.0
    local basin = -0.42 * math.exp(-(dx * dx + dz * dz) / 60.0)
    local ripple = 0.055 * math.sin(x * 0.23) * math.cos(z * 0.19)
    return ramp + plateauRoll + basin + ripple
end

local function groundHeightAt(x, z)
    if Physics and type(Physics.Raycast) == "function" then
        local ok, hit = pcall(Physics.Raycast, x, 64.0, z, 0.0, -1.0, 0.0, 96.0, 4294967295)
        if ok and hit and hit.Hit == true then
            local point = hit.Point
            local py = point and point.y
            if type(py) == "number" and (characterId == nil or hit.Entity ~= characterId) then
                return py
            end
        end
    end
    return profileGround(x, z)
end

local function pushAimWeight(weight)
    if animationServiceWarned then
        return
    end
    if Animation == nil or type(Animation.SetParameter) ~= "function" then
        animationServiceWarned = true
        print("[ANIM-SHOWCASE] Animation.SetParameter unavailable: the AnimGraphSystem maps EnableUpperBodyLayer to AimWeight, so F still works.")
        return
    end
    local ok, errorText = pcall(Animation.SetParameter, character, "AimWeight", weight)
    if not ok then
        animationServiceWarned = true
        print("[ANIM-SHOWCASE] Animation.SetParameter failed: " .. tostring(errorText))
    end
end

local function stateLabel()
    if not grounded then
        if verticalVelocity > 0.05 then
            return "跳跃"
        elseif verticalVelocity < -6.0 then
            return "快速下落"
        end
        return "下落"
    end
    if speed <= 0.12 then
        return "待机"
    end
    if math.abs(dirX) > math.abs(dirZ) then
        return dirX > 0.0 and "右平移" or "左平移"
    end
    if dirZ < -0.3 then
        return "后退"
    end
    if speed > (WALK_SPEED + 0.25) then
        return "奔跑"
    end
    return "行走"
end

local function bind()
    local foundCharacter = nil
    local foundCamera = nil
    local foundShowcase = false
    local foundAnim = nil
    local foundController = nil

    -- 一条查询同时拿到角色所需的四个组件:query 直接给字段代理,
    -- 既避免 GetComponent 的短名歧义,也保证"命中的实体真的挂齐了组件"。
    local characterQuery = ecs:Query({ "TagComponent", "TransformComponent", "AnimGraphComponent", "CharacterControllerComponent" })
    characterQuery:Each(function(entity, tag, transform, anim, controller)
        if tag.Tag == CHARACTER_TAG then
            foundCharacter = entity
            charTransform = transform
            foundAnim = anim
            foundController = controller
        elseif tag.Tag == CAMERA_TAG then
            foundCamera = entity
                local cameraLocation = transform.Location
                if cameraLocation ~= nil then
                    camX = cameraLocation.x
                    camY = cameraLocation.y
                    camZ = cameraLocation.z
                end
        elseif tag.Tag == SHOWCASE_TAG then
            foundShowcase = true
        end
    end)

    -- 相机允许不挂动画组件:只缺相机时再补一条轻量查询。
    if foundCamera == nil then
        local cameraQuery = ecs:Query({ "TagComponent", "TransformComponent" })
        cameraQuery:Each(function(entity, tag, transform)
            if tag.Tag == CAMERA_TAG then
                foundCamera = entity
                local cameraLocation = transform.Location
                if cameraLocation ~= nil then
                    camX = cameraLocation.x
                    camY = cameraLocation.y
                    camZ = cameraLocation.z
                end
            elseif tag.Tag == SHOWCASE_TAG then
                foundShowcase = true
            end
        end)
    end

    if foundCharacter == nil then
        if foundShowcase then
            print("[ANIM-SHOWCASE] Level is present but entity '" .. CHARACTER_TAG
                .. "' with AnimGraphComponent + CharacterControllerComponent was not found; showcase system idle.")
        else
            foreignScene = true
        end
        return false
    end

    character = foundCharacter
    characterId = character:GetID()
    cameraEntity = foundCamera
    charAnim = foundAnim
    charController = foundController

    if charTransform == nil then
        print("[ANIM-SHOWCASE] '" .. CHARACTER_TAG .. "' has no TransformComponent; showcase system idle.")
        foreignScene = true
        return false
    end


    if charAnim == nil and not warnedAnim then
        warnedAnim = true
        print("[ANIM-SHOWCASE] Missing AnimGraphComponent on '" .. CHARACTER_TAG .. "': movement still works, animation stays off.")
    end
    if charController == nil and not warnedController then
        warnedController = true
        print("[ANIM-SHOWCASE] Missing CharacterControllerComponent on '" .. CHARACTER_TAG .. "': AnimGraphSystem cannot derive Speed/Direction from velocity.")
    end
    if Animation == nil and not animationServiceWarned then
        animationServiceWarned = true
        print("[ANIM-SHOWCASE] Animation.SetParameter unavailable: the AnimGraphSystem maps EnableUpperBodyLayer to AimWeight, so F still works.")
    end

    bound = true
    if not startupPrinted then
        startupPrinted = true
        print("[ANIM-SHOWCASE] ============================================================")
        print("[ANIM-SHOWCASE] 动画图示例已就绪 —— 人偶移动 + 脚部 IK + 根运动")
        print("[ANIM-SHOWCASE]   WASD / 左摇杆 : 移动        Shift : 奔跑")
        print("[ANIM-SHOWCASE]   Space         : 跳跃        F     : 上半身瞄准层")
        print("[ANIM-SHOWCASE]   R             : 根运动      Tab   : 跟随 / 自由相机")
        print("[ANIM-SHOWCASE]   鼠标          : 屏幕控制条（行走/奔跑/跳跃/瞄准/根运动）")
        print("[ANIM-SHOWCASE]   提示：坡面朝世界 +X 升高（从起点按 D 即可上坡）。")
        print("[ANIM-SHOWCASE] ============================================================")
    end
    return true
end
-- Play 会克隆场景(实体句柄全部改变):在运行态启动点重新绑定,停止时解绑。
-- 代理是查询对象内的享元:进入 Play/换场景后必须重新查询,不能在旧场景上继续用。
local function unbind()
    bound = false
    character = nil
    characterId = nil
    cameraEntity = nil
    charTransform = nil
    charAnim = nil
    charController = nil
    resolveTimer = 1.0
end

ecs:AddStartupSystem("AnimationShowcaseStartup", function()
    unbind()
    foreignScene = false
    speed = 0.0
    verticalVelocity = 0.0
    grounded = true
    bind()
end)

ecs:AddTeardownSystem("AnimationShowcaseTeardown", function()
    unbind()
end)

ecs:AddSystem("AnimationShowcaseSystem", "Update", function(dt)
    if foreignScene then
        return
    end
    dt = math.min(math.max(dt, 0.0), 0.05)
    if not bound then
        resolveTimer = resolveTimer + dt
        if resolveTimer >= 0.5 then
            resolveTimer = 0.0
            bind()
        end
        if not bound then
            return
        end
    end

    -- 代理是查询对象内的享元(跨实体复用):每帧重新查询一次,保证代理指向当前场景的角色。
    local refresh = ecs:Query({ "TagComponent", "TransformComponent", "AnimGraphComponent", "CharacterControllerComponent" })
    local matchedCharacters = 0
    refresh:Each(function(entity, tag, transform, anim, controller)
        if tag.Tag == CHARACTER_TAG then
            matchedCharacters = matchedCharacters + 1
            character = entity
            charTransform = transform
            charAnim = anim
            charController = controller
        end
    end)
    if matchedCharacters == 0 then
        unbind()
        return
    end

    local location = charTransform.Location
    if location == nil then
        return
    end

    local oldX = location.x
    local oldY = location.y
    local oldZ = location.z
    local x = oldX
    local y = oldY
    local z = oldZ

    -- 一键坡面示范:把角色放到缓坡上,用于直接观察 Foot IK 的双脚贴合。
    if demoTeleport ~= nil then
        x = demoTeleport == "slope" and 18.0 or 0.0
        z = demoTeleport == "slope" and -3.0 or 0.0
        y = groundHeightAt(x, z)
        grounded = true
        verticalVelocity = 0.0
        speed = 0.0
        dirX = 0.0
        dirZ = 1.0
        camX = x
        camY = y + CAMERA_OFFSET_Y
        camZ = z + CAMERA_OFFSET_Z
        demoTeleport = nil
    end

    -- ---- 1. Input (keyboard + gamepad) -------------------------------------
    local horizontalKey = (keyDown("D", KEY_D) and 1.0 or 0.0) - (keyDown("A", KEY_A) and 1.0 or 0.0)
    local forwardKey = (keyDown("W", KEY_W) and 1.0 or 0.0) - (keyDown("S", KEY_S) and 1.0 or 0.0)
    local gamepadX = axisValue("Move")
    local gamepadZ = firstNonZero(axisValue("MoveVertical"), axisValue("MoveForward"), axisValue("MoveY"), axisValue("Vertical"))
    local inputX = horizontalKey ~= 0.0 and horizontalKey or gamepadX
    local inputZ = forwardKey ~= 0.0 and forwardKey or gamepadZ

    local inputLength = math.sqrt(inputX * inputX + inputZ * inputZ)
    if inputLength > 1.0 then
        inputX = inputX / inputLength
        inputZ = inputZ / inputLength
        inputLength = 1.0
    end
    if inputLength > 0.05 then
        -- 角色不能转身(Transform.Rotation 无 Luau 映射),所以平滑的是移动方向向量:
        -- Direction 参数会随之连续变化,2D 混合空间的 Walk/Strafe/WalkBack 过渡没有硬切.
        local targetX = inputX / inputLength
        local targetZ = inputZ / inputLength
        local turnBlend = 1.0 - math.exp(-14.0 * dt)
        dirX = dirX + (targetX - dirX) * turnBlend
        dirZ = dirZ + (targetZ - dirZ) * turnBlend
        local dirLength = math.sqrt(dirX * dirX + dirZ * dirZ)
        if dirLength > 0.0001 then
            dirX = dirX / dirLength
            dirZ = dirZ / dirLength
        end
    end

    -- 屏幕按钮:按住状态由回调翻转,这里叠加到同一套输入语义上。
    if demoMove ~= 0 then
        inputX = 0.0
        inputZ = demoMove
        inputLength = 1.0
    end

    local sprint = keyDown("LeftShift", KEY_LEFT_SHIFT) or actionDown("Sprint") or demoRun
    local targetSpeed = 0.0
    if inputLength > 0.05 then
        targetSpeed = (sprint and RUN_SPEED or WALK_SPEED) * math.min(inputLength, 1.0)
    end
    speed = approach(speed, targetSpeed, ACCELERATION, dt)
    if speed < 0.02 then
        speed = 0.0
    end

    -- ---- 2. Edge-triggered actions -----------------------------------------
    local jumpHeld = keyDown("Space", KEY_SPACE) or actionDown("Jump")
    local jumpPressed = jumpHeld and not previousJumpHeld
    previousJumpHeld = jumpHeld
    if demoJump then
        demoJump = false
        jumpPressed = true
    end
    if jumpPressed and grounded then
        verticalVelocity = JUMP_SPEED
        grounded = false
        lastEvent = "跳跃"
        lastEventTimer = 1.4
    end

    local aimHeld = keyDown("F", KEY_F) or actionPressed("Interact")
    local aimPressed = aimHeld and not previousAimHeld
    previousAimHeld = aimHeld
    if demoAimToggle then
        demoAimToggle = false
        aimPressed = true
    end
    if aimPressed then
        aim = not aim
        if charAnim ~= nil then
            charAnim.EnableUpperBodyLayer = aim and 1 or 0
        end
        pushAimWeight(aim and 1.0 or 0.0)
        lastEvent = aim and "瞄准层 开" or "瞄准层 关"
        lastEventTimer = 2.0
        print("[ANIM-SHOWCASE] Upper-body aim layer " .. (aim and "ON" or "OFF") .. " (F).")
    end

    local rootMotionHeld = keyDown("R", KEY_R)
    local rootMotionPressed = rootMotionHeld and not previousRootMotionHeld
    previousRootMotionHeld = rootMotionHeld
    if demoRootToggle then
        demoRootToggle = false
        rootMotionPressed = true
    end
    if rootMotionPressed then
        rootMotion = not rootMotion
        if charAnim ~= nil then
            charAnim.EnableRootMotion = rootMotion and 1 or 0
        end
        lastEvent = rootMotion and "根运动：动画" or "根运动：脚本"
        lastEventTimer = 2.0
        if rootMotion then
            print("[ANIM-SHOWCASE] Root Motion ON: translation now comes from the clip; if the clip has no root displacement the character stays put (expected).")
        else
            print("[ANIM-SHOWCASE] Root Motion OFF: character translation is driven by input again.")
        end
    end

    local cameraHeld = keyDown("Tab", KEY_TAB)
    local cameraPressed = cameraHeld and not previousCameraHeld
    previousCameraHeld = cameraHeld
    if demoCameraToggle then
        demoCameraToggle = false
        cameraPressed = true
    end
    if cameraPressed then
        followCamera = not followCamera
        lastEvent = followCamera and "相机：跟随" or "相机：自由"
        lastEventTimer = 2.0
        print("[ANIM-SHOWCASE] Camera " .. (followCamera and "follow" or "free (locked)") .. " (Tab).")
    end

    -- ---- 3. Horizontal motion ----------------------------------------------
    -- Root Motion 模式下不叠加脚本位移,否则会与 AnimGraphSystem 的根位移双倍移动.
    if not rootMotion then
        local stepX = dirX * speed * dt
        local stepZ = dirZ * speed * dt
        if stepX ~= 0.0 or stepZ ~= 0.0 then
            local nextX = x + stepX
            local nextZ = z + stepZ
            local aheadGround = groundHeightAt(nextX, nextZ)
            if grounded and (aheadGround - y) > MAX_STEP_UP then
                lastEvent = "台阶过高"
                lastEventTimer = 0.9
            else
                x = nextX
                z = nextZ
            end
        end
    end

    -- ---- 4. Vertical motion + ground follow --------------------------------
    x = math.max(WORLD_MIN_X, math.min(WORLD_MAX_X, x))
    z = math.max(WORLD_MIN_Z, math.min(WORLD_MAX_Z, z))

    if not grounded then
        verticalVelocity = verticalVelocity + GRAVITY * dt
        y = y + verticalVelocity * dt
    end

    local groundY = groundHeightAt(x, z)
    if grounded then
        if groundY < y - 0.5 then
            grounded = false
            verticalVelocity = 0.0
        else
            y = approach(y, groundY, GROUND_SNAP_RATE, dt)
            verticalVelocity = 0.0
        end
    elseif y <= groundY and verticalVelocity <= 0.0 then
        y = groundY
        verticalVelocity = 0.0
        grounded = true
        lastEvent = "落地"
        lastEventTimer = 1.0
    end

    if charController ~= nil then
        charController.Velocity = vec3.new(dirX * speed, verticalVelocity, dirZ * speed)
        charController.Grounded = grounded
    end

    if math.abs(x - oldX) + math.abs(y - oldY) + math.abs(z - oldZ) > 0.000001 then
        charTransform.Location = vec3.new(x, y, z)
    end

    if aim then
        pushAimWeight(1.0)
    end

    -- ---- 5. Camera follow (position only; rotation is authored) ------------
    if followCamera then
        local desiredX = x + CAMERA_OFFSET_X
        local desiredY = y + CAMERA_OFFSET_Y
        local desiredZ = z + CAMERA_OFFSET_Z
        local groundAtCamera = groundHeightAt(desiredX, desiredZ)
        if desiredY < groundAtCamera + CAMERA_MIN_CLEARANCE then
            desiredY = groundAtCamera + CAMERA_MIN_CLEARANCE
        end

        local blend = 1.0 - math.exp(-CAMERA_FOLLOW_RATE * dt)
        camX = camX + (desiredX - camX) * blend
        camY = camY + (desiredY - camY) * blend
        camZ = camZ + (desiredZ - camZ) * blend

        local cameraGround = groundHeightAt(camX, camZ)
        if camY < cameraGround + CAMERA_MIN_CLEARANCE then
            camY = cameraGround + CAMERA_MIN_CLEARANCE
        end
        -- 相机 Transform 也在查询回调里写:代理是查询对象内的享元,不能在回调外长期持有。
        local cameraQuery = ecs:Query({ "TagComponent", "TransformComponent" })
        cameraQuery:Each(function(entity, tag, transform)
            if tag.Tag == CAMERA_TAG then
                transform.Location = vec3.new(camX, camY, camZ)
            end
        end)
    end

    if lastEventTimer > 0.0 then
        lastEventTimer = math.max(0.0, lastEventTimer - dt)
        if lastEventTimer == 0.0 then
            lastEvent = "就绪"
        end
    end
end)

ui.onDraw(function()
    if not bound then
        return
    end

    ui.panel(24, 24, 392, 408, "动画图内容示例")
    ui.text(40, 62, "状态   " .. stateLabel(), 17)
    ui.text(40, 88, string.format("速度   %.2f m/s", speed), 14)
    ui.text(40, 106, "瞄准     " .. (aim and "开（上半身层）" or "关"), 14)
    ui.text(40, 124, "根运动   " .. (rootMotion and "动画根运动" or "脚本位移"), 14)
    ui.text(40, 142, "相机     " .. (followCamera and "跟随" or "自由（锁定）"), 14)
    ui.text(40, 168, "WASD 移动   Shift 奔跑   Space 跳跃", 12)
    ui.text(40, 186, "F 瞄准   R 根运动   Tab 相机", 12)
    ui.text(40, 212, "最近操作   " .. lastEvent, 12)

    -- 鼠标可玩控制条(与键盘完全等价;也用于自动化点击验证)。
    if ui.button("demo_walk", 40, 236, 104, 26, (demoMove == 1 and not demoRun) and "行走（开）" or "行走") then
        demoMove = (demoMove == 1) and 0 or 1
        lastEvent = demoMove == 1 and "控制条：行走" or "控制条：停止"
        lastEventTimer = 1.4
    end
    if ui.button("demo_run", 152, 236, 104, 26, demoRun and "奔跑（开）" or "奔跑") then
        demoRun = not demoRun
        if demoRun then demoMove = 1 end
        lastEvent = demoRun and "控制条：奔跑" or "控制条：恢复行走速度"
        lastEventTimer = 1.4
    end
    if ui.button("demo_stop", 264, 236, 104, 26, "停止") then
        demoMove = 0
        demoRun = false
        lastEvent = "控制条：停止"
        lastEventTimer = 1.0
    end
    if ui.button("demo_jump", 40, 270, 120, 26, "跳跃") then
        demoJump = true
        lastEvent = "控制条：跳跃"
        lastEventTimer = 1.0
    end
    if ui.button("demo_aim", 168, 270, 120, 26, aim and "瞄准（开）" or "瞄准") then
        demoAimToggle = true
        lastEvent = "控制条：瞄准"
        lastEventTimer = 1.0
    end
    if ui.button("demo_root", 40, 304, 156, 26, rootMotion and "根运动（开）" or "根运动") then
        demoRootToggle = true
        lastEvent = "控制条：根运动"
        lastEventTimer = 1.0
    end
    if ui.button("demo_camera", 204, 304, 156, 26, followCamera and "相机 [跟随]" or "相机 [自由]") then
        demoCameraToggle = true
        lastEvent = "控制条：相机"
        lastEventTimer = 1.0
    end
    if ui.button("demo_slope", 40, 338, 156, 26, "坡面演示（脚部 IK）") then
        demoTeleport = "slope"
        demoMove = 0
        demoRun = false
        lastEvent = "控制条：坡面脚部 IK 演示"
        lastEventTimer = 2.0
    end
    if ui.button("demo_reset", 204, 338, 156, 26, "重置位置") then
        demoTeleport = "origin"
        demoMove = 0
        demoRun = false
        lastEvent = "控制条：重置到起点"
        lastEventTimer = 2.0
    end
    ui.text(40, 378, "点击按钮，或使用上方键盘快捷键。", 11)
end)





















