-- =============================================================================
-- WorldEngine — Animation Showcase System (Milestone 4)
-- =============================================================================
-- 用户可操作的 AnimGraph 内容示例:
--   WASD / 左摇杆 LX 移动, Shift 奔跑, Space 跳跃,
--   F 瞄准(上半身层), R 切换 Root Motion, Tab 切换跟随/自由视角,
--   G 切换手部 IK, 鼠标 / 方向键 旋转第三人称轨道镜头,
--   滚轮缩放, C 切换角色朝向模式, Esc/M 释放或重新捕获指针.
--
-- 设计口径:
--   * 脚本只写 CharacterControllerComponent.Velocity / Grounded;
--     Speed / Direction / IsFalling 由 AnimGraphSystem 从 Velocity 推导.
--     脚本绝不写 AnimGraphComponent.CurrentStateHash.
--   * 角色朝向通过 Transform.Rotation 写入 quat.fromEuler(0, yaw, 0).
--     移动相对镜头: W = 镜头前方, D = 屏幕右侧; Direction 由 AnimGraphSystem
--     按角色局部速度推导(面向 +Z = 0°, 角色右侧 = +90°), 脚本不自行取反.
--   * 角色朝向有三种模式: 跟随镜头 / 朝向移动方向 / 固定朝向(+Z).
--     轨道镜头绕角色旋转; 位置指数平滑, 旋转直接跟手.
--   * 地面高度优先取 Physics.Raycast(地形以后补上碰撞即自动生效);
--     否则按 animation_showcase.wterrain 的生成公式解析求值(与
--     Engine/generators/mannequin/generate_showcase_terrain.py 逐项一致).
--   * 找不到本关实体时静默退出,不打扰其它场景; 组件缺失只提示一次.
-- =============================================================================

local CHARACTER_TAG = "Mannequin Character"
local CAMERA_TAG = "Showcase Camera"
local SHOWCASE_TAG = "Showcase Terrain"
local RIGHT_HAND_TARGET_TAG = "Right Hand IK Target"
local RIGHT_HAND_IK_TAG = "Right Hand IK"

local WALK_SPEED = 1.6          -- m/s
local RUN_SPEED = 4.5           -- m/s
local ACCELERATION = 12.0       -- 1/s, exponential approach
local JUMP_SPEED = 5.2          -- m/s
local GRAVITY = -18.0           -- m/s^2
local GROUND_SNAP_RATE = 18.0   -- 1/s
local CAMERA_DEFAULT_DISTANCE = 4.5
local CAMERA_MIN_DISTANCE = 2.6
local CAMERA_MAX_DISTANCE = 7.0
local CAMERA_INITIAL_PITCH_DEG = 12.0
local CAMERA_MIN_PITCH_DEG = -60.0
local CAMERA_MAX_PITCH_DEG = 70.0
local CAMERA_TARGET_HEIGHT = 1.55 -- 角色脚底到镜头观察点(胸口附近)
local CAMERA_FOLLOW_RATE = 6.0    -- 1/s (dispatch: k ~= 6)
local CAMERA_DISTANCE_RATE = 10.0 -- 1/s, 滚轮距离平滑
local CAMERA_MOUSE_SENSITIVITY = 0.12 -- 度/像素
local CAMERA_KEY_YAW_RATE = 100.0     -- 度/秒
local CAMERA_KEY_PITCH_RATE = 70.0    -- 度/秒
local CAMERA_BUTTON_YAW_STEP = 15.0   -- 度/次
local CAMERA_SCROLL_STEP = 0.45       -- m/滚轮刻度
local CAMERA_MIN_CLEARANCE = 0.8
local CHARACTER_TURN_RATE = 12.0      -- 1/s, 角色朝向最短角插值
local MAX_STEP_UP = 0.45        -- m, higher than StepOffset blocks the move
local WORLD_MIN_X = -22.0       -- keep the character on the 48 m showcase slab/terrain
local WORLD_MAX_X = 38.0
local WORLD_MIN_Z = -22.0
local WORLD_MAX_Z = 38.0

-- 手部 IK 示例目标: 角色右前方的一条小 8 字轨迹.
-- 基准点与 910000000000000014.entity 的初始位置一致(角色在原点、朝向 +Z 时).
local HAND_MOTION_RATE = 1.2          -- rad/s
local HAND_BASE_RIGHT = 0.55          -- m, 角色右侧(-X)偏移
local HAND_BASE_FORWARD = 0.45        -- m, 角色前方(+Z)偏移
local HAND_BASE_HEIGHT = 1.25         -- m, 相对角色脚底
local HAND_SWAY_LATERAL = 0.07        -- m, 左右摆动幅度
local HAND_SWAY_FORWARD = 0.16        -- m, 前后摆动幅度
local HAND_SWAY_HEIGHT = 0.10         -- m, 0..10 cm 小起伏

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
local KEY_G = 71
local KEY_R = 82
local KEY_TAB = 258
local KEY_C = 67
local KEY_M = 77
local KEY_ESCAPE = 256
local KEY_RIGHT = 262
local KEY_LEFT = 263
local KEY_DOWN = 264
local KEY_UP = 265

local ORIENTATION_FOLLOW_CAMERA = 1
local ORIENTATION_MOVE_DIRECTION = 2
local ORIENTATION_FIXED = 3
local ORIENTATION_LABELS = {
    "跟随镜头",
    "朝向移动",
    "固定朝向",
}

local character = nil
local characterId = nil
local cameraEntity = nil
local charTransform = nil
local charAnim = nil
local charController = nil
local rightHandTargetEntity = nil
local rightHandIkEntity = nil

local bound = false
local foreignScene = false
local resolveTimer = 1.0
local warnedAnim = false
local warnedController = false
local animationServiceWarned = false
local startupPrinted = false
local warnedHandIk = false

local speed = 0.0
local dirX = 0.0
local dirZ = 1.0
local charYaw = 0.0             -- 弧度, 角色绕 Y 的朝向
local charRotationApplied = nil -- 最近一次真正写入 Transform.Rotation 的 yaw
local verticalVelocity = 0.0
local grounded = true
local aim = false
local rootMotion = false
local footIkEnabled = false
local handIkEnabled = true
local handPhase = 0.0
local followCamera = true
local camYaw = 0.0              -- 弧度, 镜头观察方向(角色前方 = 镜头前方)
local camPitch = math.rad(CAMERA_INITIAL_PITCH_DEG) -- 弧度, 正值 = 镜头升高俯视
local cameraDistance = CAMERA_DEFAULT_DISTANCE
local cameraDistanceTarget = CAMERA_DEFAULT_DISTANCE
local camX = 0.0               -- 平滑后的实际相机位置
local camY = 0.0
local camZ = 0.0
local cameraSnapPending = true
local orientationMode = ORIENTATION_FOLLOW_CAMERA
local previousJumpHeld = false
local previousAimHeld = false
local previousRootMotionHeld = false
local previousHandIkHeld = false
local previousCameraHeld = false
local previousOrientationHeld = false
local lastEvent = "就绪"
local lastEventTimer = 0.0
local warnedQuat = false
local warnedMouseDelta = false

-- 屏幕控制条(鼠标可玩):与键盘输入等价,同时给自动化一个可点击的用户入口。
local demoMove = 0          -- 0 = 停止, 1 = 前进, -1 = 后退
local demoRun = false
local demoJump = false
local demoAimToggle = false
local demoRootToggle = false
local demoHandIkToggle = false
local demoCameraToggle = false
local demoOrientationToggle = false
local demoCameraLeft = false
local demoCameraRight = false
local demoCameraReset = false
local demoTeleport = nil       -- "slope" | "origin"

-- 光标策略（对齐主流第三人称游戏）：
--   * 进入 Play 立即捕获并隐藏指针，鼠标直接控镜头；
--   * Esc / M 释放指针并显示，便于点击 HUD 或编辑器按钮；
--   * 再次按 Esc / M 重新捕获；捕获后鼠标直接控镜头，不需要按住任何鼠标键。
-- 捕获态下用原始鼠标增量，指针不会撞到窗口边缘，也不会离开游戏窗口。
local cursorLocked = false
local cursorHidden = false
-- 编辑器 Play 会在启动脚本之前就捕获并隐藏指针（引擎级 PIE 策略）。
-- 此时 Esc 的释放/重捕获由编辑器负责，脚本只跟随状态，避免同一次按键被处理两遍。
local engineOwnsPointer = false
local previousCursorToggleHeld = false
local suppressLookFrame = false -- 捕获/释放那一帧的指针跳变不能算进镜头

local function setCursorLocked(locked)
    if cursorLocked == locked then
        return
    end
    cursorLocked = locked
    if type(Input.SetCursorLocked) == "function" then
        pcall(Input.SetCursorLocked, locked)
    end
end

local function setCursorHidden(hidden)
    if cursorHidden == hidden then
        return
    end
    cursorHidden = hidden
    if type(Input.SetCursorVisible) == "function" then
        pcall(Input.SetCursorVisible, not hidden)
    end
end

local function captureCursor()
    setCursorHidden(true)
    setCursorLocked(true)
    suppressLookFrame = true
end

local function releaseCursor()
    setCursorLocked(false)
    setCursorHidden(false)
    suppressLookFrame = true
end

local function engineCursorLocked()
    if type(Input.CursorLocked) ~= "function" then
        return nil
    end
    local ok, value = pcall(Input.CursorLocked)
    if ok and type(value) == "boolean" then
        return value
    end
    return nil
end

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

local function mouseDelta()
    if type(Input.MouseDelta) ~= "function" then
        if not warnedMouseDelta then
            warnedMouseDelta = true
            print("[ANIM-SHOWCASE] Input.MouseDelta unavailable: mouse look is disabled until the engine binding lands.")
        end
        return 0.0, 0.0
    end
    local ok, value = pcall(Input.MouseDelta)
    if ok and value ~= nil then
        local dx = value.x
        local dy = value.y
        if type(dx) == "number" and type(dy) == "number" then
            -- 防止窗口重建/指针跳变把镜头甩飞。
            return math.max(-200.0, math.min(200.0, dx)), math.max(-200.0, math.min(200.0, dy))
        end
    end
    return 0.0, 0.0
end

local function scrollValue()
    if type(Input.Scroll) ~= "function" then
        return 0.0
    end
    local ok, value = pcall(Input.Scroll)
    if ok and type(value) == "number" then
        return value
    end
    return 0.0
end

local function setRotation(transform, xDeg, yDeg, zDeg)
    if type(quat) == "nil" or type(quat.fromEuler) ~= "function" then
        if not warnedQuat then
            warnedQuat = true
            print("[ANIM-SHOWCASE] quat.fromEuler unavailable: character/camera rotation writes are skipped until the engine binding lands.")
        end
        return false
    end
    local ok, errorText = pcall(function()
        transform.Rotation = quat.fromEuler(xDeg, yDeg, zDeg)
    end)
    if not ok and not warnedQuat then
        warnedQuat = true
        print("[ANIM-SHOWCASE] Transform.Rotation write failed: " .. tostring(errorText))
    end
    return ok
end

local function wrapDegrees(degrees)
    local wrapped = math.fmod(degrees + 180.0, 360.0)
    if wrapped < 0.0 then
        wrapped = wrapped + 360.0
    end
    return wrapped - 180.0
end

local function shortestAngle(delta)
    return math.atan2(math.sin(delta), math.cos(delta))
end

local function clampCameraPitch(pitch)
    return math.max(math.rad(CAMERA_MIN_PITCH_DEG), math.min(math.rad(CAMERA_MAX_PITCH_DEG), pitch))
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
    -- 平移标签按角色自身左右判断: 先把世界方向转回角色局部坐标。
    local cosYaw = math.cos(charYaw)
    local sinYaw = math.sin(charYaw)
    local localX = dirX * cosYaw - dirZ * sinYaw
    local localZ = dirX * sinYaw + dirZ * cosYaw
    local localRight = -localX -- Direction 语义: 面向 +Z 时角色右侧 = +90° = 局部 -X
    if math.abs(localRight) > math.abs(localZ) then
        return localRight > 0.0 and "右平移" or "左平移"
    end
    if localZ < -0.3 then
        return "后退"
    end
    if speed > (WALK_SPEED + 0.25) then
        return "奔跑"
    end
    return "行走"
end

local function orientationLabel()
    return ORIENTATION_LABELS[orientationMode]
end

local function cameraModeLabel()
    return followCamera and "跟随" or "自由（锁定）"
end

local function bind()
    local foundCharacter = nil
    local foundCamera = nil
    local foundShowcase = false
    local foundAnim = nil
    local foundController = nil
    local foundHandTarget = nil
    local foundHandIk = nil

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

    -- 手部 IK 示例: 目标只取 Transform, 代理实体只读取 TwoBoneIkComponent。
    -- 两条查询都按 Tag 过滤, 因而不会和其它场景中的 IK 链混淆。
    local handTargetQuery = ecs:Query({ "TagComponent", "TransformComponent" })
    handTargetQuery:Each(function(entity, tag)
        if tag.Tag == RIGHT_HAND_TARGET_TAG then
            foundHandTarget = entity
        end
    end)

    local handIkQuery = ecs:Query({ "TagComponent", "TwoBoneIkComponent" })
    handIkQuery:Each(function(entity, tag, ik)
        if tag.Tag == RIGHT_HAND_IK_TAG then
            foundHandIk = entity
            handIkEnabled = ik.Enabled ~= 0
        end
    end)

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
    rightHandTargetEntity = foundHandTarget
    rightHandIkEntity = foundHandIk
    if rightHandIkEntity == nil then
        handIkEnabled = false
    end

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
    if (rightHandTargetEntity == nil or rightHandIkEntity == nil) and not warnedHandIk then
        warnedHandIk = true
        print("[ANIM-SHOWCASE] Hand IK example is incomplete: expected '" .. RIGHT_HAND_TARGET_TAG
            .. "' (Transform) and '" .. RIGHT_HAND_IK_TAG .. "' (TwoBoneIkComponent); G toggle is idle.")
    end
    if Animation == nil and not animationServiceWarned then
        animationServiceWarned = true
        print("[ANIM-SHOWCASE] Animation.SetParameter unavailable: the AnimGraphSystem maps EnableUpperBodyLayer to AimWeight, so F still works.")
    end

    bound = true
    charRotationApplied = nil
    if not startupPrinted then
        startupPrinted = true
        print("[ANIM-SHOWCASE] ============================================================")
        print("[ANIM-SHOWCASE] 动画图示例已就绪 —— 第三人称镜头 + 朝向体系 + 脚部 IK")
        print("[ANIM-SHOWCASE]   WASD / 左摇杆 : 移动        Shift : 奔跑")
        print("[ANIM-SHOWCASE]   Space         : 跳跃        F     : 上半身瞄准层")
        print("[ANIM-SHOWCASE]   R             : 根运动      Tab   : 跟随 / 自由镜头")
        print("[ANIM-SHOWCASE]   鼠标移动/方向键: 旋转镜头（上推 = 抬头）  滚轮 : 缩放")
        print("[ANIM-SHOWCASE]   C             : 朝向模式（跟随镜头 / 朝向移动 / 固定）")
        print("[ANIM-SHOWCASE]   G             : 手部 IK 开 / 关（右手跟随青色目标）")
        print("[ANIM-SHOWCASE]   Esc / M       : 释放 / 重新捕获指针（释放后可点 HUD 按钮）")
        print("[ANIM-SHOWCASE] 指针归属: " .. (engineOwnsPointer and "编辑器 Play(引擎持有)" or "脚本持有"))
        print("[ANIM-SHOWCASE]   提示：坡面朝世界 +X 升高（从起点按 A 即可上坡(向左)或转向推 W）。")
        print("[ANIM-SHOWCASE] ============================================================")
    end
    return true
end
-- Play 会克隆场景(实体句柄全部改变):在运行态启动点重新绑定,停止时解绑。
-- 代理是查询对象内的享元:进入 Play/换场景后必须重新查询,不能在旧场景上继续用。
local function unbind(skipCursorRelease)
    bound = false
    -- 停止 Play / 切场景时务必把系统光标还回去，否则编辑器里鼠标会一直消失。
    -- 编辑器自己持有指针时由它的退出路径还原，脚本不越权。
    if not skipCursorRelease and not engineOwnsPointer then
        releaseCursor()
    end
    character = nil
    characterId = nil
    cameraEntity = nil
    charTransform = nil
    charAnim = nil
    charController = nil
    rightHandTargetEntity = nil
    rightHandIkEntity = nil
    resolveTimer = 1.0
end

ecs:AddStartupSystem("AnimationShowcaseStartup", function()
    -- 必须在 unbind 之前判定归属：编辑器 Play 会先捕获指针，若先 release 再判断，
    -- 脚本会误以为"编辑器没接管"，从而和编辑器各管一半（Esc 会被切换两次）。
    local engineAlreadyCaptured = engineCursorLocked() == true
    unbind(engineAlreadyCaptured)
    foreignScene = false
    speed = 0.0
    dirX = 0.0
    dirZ = 1.0
    charYaw = 0.0
    charRotationApplied = nil
    verticalVelocity = 0.0
    grounded = true
    camYaw = 0.0
    camPitch = math.rad(CAMERA_INITIAL_PITCH_DEG)
    cameraDistance = CAMERA_DEFAULT_DISTANCE
    cameraDistanceTarget = CAMERA_DEFAULT_DISTANCE
    cameraSnapPending = true
    orientationMode = ORIENTATION_FOLLOW_CAMERA
    followCamera = true
    footIkEnabled = false
    handIkEnabled = true
    handPhase = 0.0
    previousHandIkHeld = false
    demoHandIkToggle = false
    -- 编辑器 Play 已在脚本启动前捕获指针；Runtime（无编辑器）由脚本自己捕获。
    engineOwnsPointer = engineAlreadyCaptured
    if engineOwnsPointer then
        cursorLocked = true
        cursorHidden = true
    else
        captureCursor()
    end
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

    -- 脚步 IK 现在由角色实体上的 FootIkComponent 驱动; HUD 读取它的实时开关。
    local footFound = false
    local footQuery = ecs:Query({ "TagComponent", "FootIkComponent" })
    footQuery:Each(function(entity, tag, foot)
        if tag.Tag == CHARACTER_TAG then
            footFound = true
            footIkEnabled = foot.Enabled ~= 0
        end
    end)
    if not footFound then
        footIkEnabled = false
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
        cameraSnapPending = true
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

    -- 屏幕按钮:按住状态由回调翻转,这里叠加到同一套输入语义上。
    if demoMove ~= 0 then
        inputX = 0.0
        inputZ = demoMove
        inputLength = 1.0
    end

    -- ---- 1.5 Third-person orbit camera input -------------------------------
    local yawDeltaDeg = 0.0
    local pitchDeltaDeg = 0.0
    if keyDown("Left", KEY_LEFT) then
        yawDeltaDeg = yawDeltaDeg + CAMERA_KEY_YAW_RATE * dt
    end
    if keyDown("Right", KEY_RIGHT) then
        yawDeltaDeg = yawDeltaDeg - CAMERA_KEY_YAW_RATE * dt
    end
    -- camPitch 为正 = 镜头升高俯视。所以"抬头"= 减小 pitch、"低头"= 增大 pitch。
    if keyDown("Up", KEY_UP) then
        pitchDeltaDeg = pitchDeltaDeg - CAMERA_KEY_PITCH_RATE * dt
    end
    if keyDown("Down", KEY_DOWN) then
        pitchDeltaDeg = pitchDeltaDeg + CAMERA_KEY_PITCH_RATE * dt
    end

    -- 指针状态每帧与引擎对齐：编辑器 Play / Stop / 切窗口都会改它。
    local engineLocked = engineCursorLocked()
    if engineLocked ~= nil then
        cursorLocked = engineLocked
    end
    -- 编辑器持有指针时，Esc 的切换由编辑器处理（脚本再处理一次会立刻切回去）。
    if not engineOwnsPointer then
        local cursorToggleHeld = keyDown("Escape", KEY_ESCAPE) or keyDown("M", KEY_M)
        if cursorToggleHeld and not previousCursorToggleHeld then
            if cursorLocked then
                releaseCursor()
                lastEvent = "指针已释放（Esc / M 重新捕获，可点击 HUD）"
            else
                captureCursor()
                lastEvent = "指针已捕获（Esc / M 释放）"
            end
            lastEventTimer = 1.8
        end
        previousCursorToggleHeld = cursorToggleHeld
    end

    -- 捕获态下鼠标直接转镜头（不需要按住任何键）；捕获/释放那一帧的指针跳变要丢弃。
    if cursorLocked and not suppressLookFrame then
        local dx, dy = mouseDelta()
        -- 屏幕右移 = 镜头向右转(camYaw 减小)。
        -- dy 向下为正：鼠标上推(dy<0)应抬头 ⇒ pitch 减小（旧实现用了 -dy，上下是反的）。
        yawDeltaDeg = yawDeltaDeg - dx * CAMERA_MOUSE_SENSITIVITY
        pitchDeltaDeg = pitchDeltaDeg + dy * CAMERA_MOUSE_SENSITIVITY
    end
    suppressLookFrame = false
    if demoCameraLeft then
        demoCameraLeft = false
        yawDeltaDeg = yawDeltaDeg + CAMERA_BUTTON_YAW_STEP
    end
    if demoCameraRight then
        demoCameraRight = false
        yawDeltaDeg = yawDeltaDeg - CAMERA_BUTTON_YAW_STEP
    end

    camYaw = camYaw + math.rad(yawDeltaDeg)
    camPitch = clampCameraPitch(camPitch + math.rad(pitchDeltaDeg))

    local wheel = scrollValue()
    if wheel ~= 0.0 then
        cameraDistanceTarget = math.max(CAMERA_MIN_DISTANCE, math.min(CAMERA_MAX_DISTANCE,
            cameraDistanceTarget - wheel * CAMERA_SCROLL_STEP))
    end
    cameraDistance = approach(cameraDistance, cameraDistanceTarget, CAMERA_DISTANCE_RATE, dt)

    if demoCameraReset then
        demoCameraReset = false
        camYaw = 0.0
        camPitch = math.rad(CAMERA_INITIAL_PITCH_DEG)
        cameraDistanceTarget = CAMERA_DEFAULT_DISTANCE
        cameraDistance = CAMERA_DEFAULT_DISTANCE
        followCamera = true
        cameraSnapPending = true
        lastEvent = "控制条：镜头复位"
        lastEventTimer = 1.5
    end

    -- 相机观察前向 = (sin yaw, 0, cos yaw); 屏幕右 = cross(forward, up)
    -- 本工程相机局部前向为 -Z, 所以右向是 (-cos yaw, 0, sin yaw), 与 Direction 的
    -- “面向 +Z 时角色右侧 = -X”一致。
    local forwardX = math.sin(camYaw)
    local forwardZ = math.cos(camYaw)
    local rightX = -math.cos(camYaw)
    local rightZ = math.sin(camYaw)

    if inputLength > 0.05 then
        local targetX = forwardX * inputZ + rightX * inputX
        local targetZ = forwardZ * inputZ + rightZ * inputX
        local targetLength = math.sqrt(targetX * targetX + targetZ * targetZ)
        if targetLength > 0.0001 then
            targetX = targetX / targetLength
            targetZ = targetZ / targetLength
            local turnBlend = 1.0 - math.exp(-16.0 * dt)
            dirX = dirX + (targetX - dirX) * turnBlend
            dirZ = dirZ + (targetZ - dirZ) * turnBlend
            local dirLength = math.sqrt(dirX * dirX + dirZ * dirZ)
            if dirLength > 0.0001 then
                dirX = dirX / dirLength
                dirZ = dirZ / dirLength
            end
        end
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
        lastEvent = followCamera and "镜头：跟随" or "镜头：自由（锁定）"
        lastEventTimer = 2.0
        print("[ANIM-SHOWCASE] Camera " .. (followCamera and "follow" or "free (locked)") .. " (Tab).")
    end

    local orientationHeld = keyDown("C", KEY_C)
    local orientationPressed = orientationHeld and not previousOrientationHeld
    previousOrientationHeld = orientationHeld
    if demoOrientationToggle then
        demoOrientationToggle = false
        orientationPressed = true
    end
    if orientationPressed then
        orientationMode = orientationMode % #ORIENTATION_LABELS + 1
        lastEvent = "朝向：" .. orientationLabel()
        lastEventTimer = 2.0
        print("[ANIM-SHOWCASE] Orientation mode: " .. orientationLabel() .. " (C).")
    end

    -- G / 控制条: 只切换右手链的 TwoBoneIkComponent.Enabled, 不碰动画状态机。
    local handIkHeld = keyDown("G", KEY_G)
    local handIkPressed = handIkHeld and not previousHandIkHeld
    previousHandIkHeld = handIkHeld
    if demoHandIkToggle then
        demoHandIkToggle = false
        handIkPressed = true
    end
    if handIkPressed then
        local handToggleApplied = false
        local handIkQuery = ecs:Query({ "TagComponent", "TwoBoneIkComponent" })
        handIkQuery:Each(function(entity, tag, ik)
            if tag.Tag == RIGHT_HAND_IK_TAG then
                handIkEnabled = not handIkEnabled
                ik.Enabled = handIkEnabled and 1 or 0
                handToggleApplied = true
            end
        end)
        if handToggleApplied then
            lastEvent = handIkEnabled and "手部 IK：开" or "手部 IK：关"
            lastEventTimer = 2.0
            print("[ANIM-SHOWCASE] Hand IK " .. (handIkEnabled and "ON" or "OFF") .. " (G).")
        else
            lastEvent = "未找到手部 IK 实体"
            lastEventTimer = 2.0
            if not warnedHandIk then
                warnedHandIk = true
                print("[ANIM-SHOWCASE] Cannot toggle hand IK: entity '" .. RIGHT_HAND_IK_TAG
                    .. "' with TwoBoneIkComponent was not found.")
            end
        end
    end

    -- ---- 3. Horizontal motion ----------------------------------------------
    -- Root Motion 模式下不叠加脚本位移,否则会与 AnimGraphSystem 的根位移双倍移动.
    if not rootMotion then
        local stepX = dirX * speed * dt
        local stepZ = dirZ * speed * dt
        if stepX ~= 0.0 or stepZ ~= 0.0 then
            local nextX = x + stepX
            local nextZ = z + stepZ
            local moveLen = math.sqrt(stepX * stepX + stepZ * stepZ)
            local blocked = false
            if Physics and type(Physics.Raycast) == "function" and moveLen > 0.0001 then
                local ok, hit = pcall(Physics.Raycast, x, y + 0.8, z, dirX, 0.0, dirZ, moveLen + 0.35, 4294967295)
                if ok and hit and hit.Hit and (characterId == nil or hit.Entity ~= characterId) then
                    -- 仅当撞到陡峭立面(如柱子侧面/高台阶)时才阻挡；地形坡面/地面法线偏向上(y >= 0.6)是可行走坡面，不应阻挡前进
                    local ny = hit.Normal and (hit.Normal.y or (hit.normal and hit.normal.y)) or 0.0
                    if ny < 0.6 then
                        blocked = true
                    end
                end
            end
            local aheadGround = groundHeightAt(nextX, nextZ)
            if blocked then
                -- 遇障碍物阻挡(如立柱/台阶侧面): 保持在原位
            elseif grounded and (aheadGround - y) > MAX_STEP_UP then
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

    -- ---- 4.5 Character facing modes ----------------------------------------
    local targetCharYaw = charYaw
    if orientationMode == ORIENTATION_FOLLOW_CAMERA then
        targetCharYaw = camYaw
    elseif orientationMode == ORIENTATION_MOVE_DIRECTION then
        if inputLength > 0.05 then
            targetCharYaw = math.atan2(dirX, dirZ)
        end
    else
        targetCharYaw = 0.0
    end

    local yawBlend = 1.0 - math.exp(-CHARACTER_TURN_RATE * dt)
    charYaw = charYaw + shortestAngle(targetCharYaw - charYaw) * yawBlend
    if charRotationApplied == nil or
        math.abs(shortestAngle(charYaw - charRotationApplied)) > 0.0001 then
        if setRotation(charTransform, 0.0, math.deg(charYaw), 0.0) then
            charRotationApplied = charYaw
        end
    end

    if charController ~= nil then
        charController.Velocity = vec3.new(dirX * speed, verticalVelocity, dirZ * speed)
        charController.Grounded = grounded
    end

    if math.abs(x - oldX) + math.abs(y - oldY) + math.abs(z - oldZ) > 0.000001 then
        charTransform.Location = vec3.new(x, y, z)
    end

    -- ---- 4.6 Hand IK target motion -----------------------------------------
    -- 目标跟随角色的右前方(而不是世界固定点), 因此转身/移动后右手仍追得到。
    -- 水平面是 1:2 频率的 8 字, 垂直做 0..10 cm 的小起伏; 各轴幅度均 <= 0.25 m。
    handPhase = handPhase + dt * HAND_MOTION_RATE
    if handPhase > math.pi * 2.0 then
        handPhase = handPhase - math.pi * 2.0
    end
    local handLateral = HAND_BASE_RIGHT + HAND_SWAY_LATERAL * math.sin(handPhase)
    local handForward = HAND_BASE_FORWARD + HAND_SWAY_FORWARD * math.sin(handPhase * 2.0)
    local handHeight = y + HAND_BASE_HEIGHT + HAND_SWAY_HEIGHT * (0.5 - 0.5 * math.cos(handPhase * 2.0))
    local charRightX = -math.cos(charYaw)
    local charRightZ = math.sin(charYaw)
    local charForwardX = math.sin(charYaw)
    local charForwardZ = math.cos(charYaw)
    local handTargetX = x + charRightX * handLateral + charForwardX * handForward
    local handTargetZ = z + charRightZ * handLateral + charForwardZ * handForward
    local handTargetQuery = ecs:Query({ "TagComponent", "TransformComponent" })
    handTargetQuery:Each(function(entity, tag, transform)
        if tag.Tag == RIGHT_HAND_TARGET_TAG then
            transform.Location = vec3.new(handTargetX, handHeight, handTargetZ)
        end
    end)

    if aim then
        pushAimWeight(1.0)
    end

    -- ---- 5. Third-person orbit camera --------------------------------------
    local orbitHorizontal = cameraDistance * math.cos(camPitch)
    local orbitVertical = cameraDistance * math.sin(camPitch)
    local desiredX = x - math.sin(camYaw) * orbitHorizontal
    local desiredY = y + CAMERA_TARGET_HEIGHT + orbitVertical
    local desiredZ = z - math.cos(camYaw) * orbitHorizontal

    local groundAtCamera = groundHeightAt(desiredX, desiredZ)
    if desiredY < groundAtCamera + CAMERA_MIN_CLEARANCE then
        desiredY = groundAtCamera + CAMERA_MIN_CLEARANCE
    end

    if followCamera then
        if cameraSnapPending then
            camX = desiredX
            camY = desiredY
            camZ = desiredZ
            cameraSnapPending = false
        else
            local blend = 1.0 - math.exp(-CAMERA_FOLLOW_RATE * dt)
            camX = camX + (desiredX - camX) * blend
            camY = camY + (desiredY - camY) * blend
            camZ = camZ + (desiredZ - camZ) * blend
        end

        local cameraGround = groundHeightAt(camX, camZ)
        if camY < cameraGround + CAMERA_MIN_CLEARANCE then
            camY = cameraGround + CAMERA_MIN_CLEARANCE
        end

        -- 相机 Transform 也在查询回调里写:代理是查询对象内的享元,不能在回调外长期持有。
        -- 本工程相机局部前向 = -Z: 观察 yaw 需要 +180°;camPitch 为正表示镜头升高俯视,
        -- 因此欧拉 X 取 -camPitch(与场景原机位 [-12°, 180°] 一致)。
        local cameraQuery = ecs:Query({ "TagComponent", "TransformComponent" })
        cameraQuery:Each(function(entity, tag, transform)
            if tag.Tag == CAMERA_TAG then
                transform.Location = vec3.new(camX, camY, camZ)
                setRotation(transform, -math.deg(camPitch), math.deg(camYaw) + 180.0, 0.0)
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

    ui.panel(24, 24, 392, 504, "动画图内容示例")
    ui.text(40, 62, "状态   " .. stateLabel(), 17)
    ui.text(40, 88, string.format("速度   %.2f m/s", speed), 14)
    ui.text(40, 106, "瞄准     " .. (aim and "开（上半身层）" or "关"), 14)
    ui.text(40, 124, "根运动   " .. (rootMotion and "动画根运动" or "脚本位移"), 14)
    ui.text(40, 142, string.format("镜头   %s  偏航 %d°  俯仰 %d°  %.1f m",
        cameraModeLabel(), math.floor(wrapDegrees(math.deg(camYaw)) + 0.5),
        math.floor(math.deg(camPitch) + 0.5), cameraDistance), 12)
    ui.text(40, 160, "朝向   " .. orientationLabel(), 14)
    ui.text(40, 178, string.format("IK   脚部[%s] 手部[%s]",
        footIkEnabled and "开" or "关", handIkEnabled and "开" or "关"), 14)
    ui.text(40, 200, "WASD 移动   Shift 奔跑   Space 跳跃", 12)
    ui.text(40, 218, "鼠标 / 方向键 转镜头   滚轮 缩放   Esc / M 释放指针", 12)
    ui.text(40, 236, "F 瞄准   G 手部 IK   R 根运动   Tab 镜头   C 朝向", 12)
    ui.text(40, 256, "最近操作   " .. lastEvent, 12)

    -- 鼠标可玩控制条(与键盘完全等价;也用于自动化点击验证)。
    if ui.button("demo_walk", 40, 280, 104, 26, (demoMove == 1 and not demoRun) and "行走（开）" or "行走") then
        demoMove = (demoMove == 1) and 0 or 1
        lastEvent = demoMove == 1 and "控制条：行走" or "控制条：停止"
        lastEventTimer = 1.4
    end
    if ui.button("demo_run", 152, 280, 104, 26, demoRun and "奔跑（开）" or "奔跑") then
        demoRun = not demoRun
        if demoRun then demoMove = 1 end
        lastEvent = demoRun and "控制条：奔跑" or "控制条：恢复行走速度"
        lastEventTimer = 1.4
    end
    if ui.button("demo_stop", 264, 280, 104, 26, "停止") then
        demoMove = 0
        demoRun = false
        lastEvent = "控制条：停止"
        lastEventTimer = 1.0
    end
    if ui.button("demo_jump", 40, 314, 120, 26, "跳跃") then
        demoJump = true
        lastEvent = "控制条：跳跃"
        lastEventTimer = 1.0
    end
    if ui.button("demo_aim", 168, 314, 120, 26, aim and "瞄准（开）" or "瞄准") then
        demoAimToggle = true
        lastEvent = "控制条：瞄准"
        lastEventTimer = 1.0
    end
    if ui.button("demo_root", 40, 348, 156, 26, rootMotion and "根运动（开）" or "根运动") then
        demoRootToggle = true
        lastEvent = "控制条：根运动"
        lastEventTimer = 1.0
    end
    if ui.button("demo_camera", 204, 348, 156, 26, followCamera and "镜头 [跟随]" or "镜头 [自由]") then
        demoCameraToggle = true
        lastEvent = "控制条：镜头跟随/自由"
        lastEventTimer = 1.0
    end
    if ui.button("demo_slope", 40, 382, 156, 26, "坡面演示（脚部 IK）") then
        demoTeleport = "slope"
        demoMove = 0
        demoRun = false
        lastEvent = "控制条：坡面脚部 IK 演示"
        lastEventTimer = 2.0
    end
    if ui.button("demo_reset", 204, 382, 156, 26, "重置位置") then
        demoTeleport = "origin"
        demoMove = 0
        demoRun = false
        lastEvent = "控制条：重置到起点"
        lastEventTimer = 2.0
    end
    if ui.button("demo_cam_left", 40, 416, 104, 26, "镜头左转") then
        demoCameraLeft = true
        lastEvent = "控制条：镜头左转"
        lastEventTimer = 1.0
    end
    if ui.button("demo_cam_right", 152, 416, 104, 26, "镜头右转") then
        demoCameraRight = true
        lastEvent = "控制条：镜头右转"
        lastEventTimer = 1.0
    end
    if ui.button("demo_orientation", 264, 416, 104, 26, "朝向模式") then
        demoOrientationToggle = true
        lastEvent = "控制条：切换朝向模式"
        lastEventTimer = 1.0
    end
    if ui.button("demo_cam_reset", 40, 450, 156, 26, "镜头复位") then
        demoCameraReset = true
        lastEvent = "控制条：镜头复位"
        lastEventTimer = 1.0
    end
    if ui.button("demo_hand_ik", 40, 484, 320, 26, handIkEnabled and "手部 IK（开）" or "手部 IK（关）") then
        demoHandIkToggle = true
        lastEvent = "控制条：手部 IK"
        lastEventTimer = 1.0
    end
    ui.text(40, 512, "点击按钮，或使用上方键盘快捷键（G 切换手部 IK）。", 11)
end)





















