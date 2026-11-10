-- =============================================================================
-- 3D 贴花与凸包物理分解综合展示关卡系统 (DecalConvexShowcaseSystem.luau)
-- =============================================================================
-- 本系统演示 WorldEngine 里程碑 7 核心特性:
--   1. 3D 投影贴花 (Projected Decals):
--      - 墙面弹孔 (Bullet Hole Cluster)
--      - 地面血斑 (Blood Splatter)
--      - 倾斜斜坡掠射角防拉伸消隐 (Normal Angle Cutoff)
--   2. Jolt 凸包物理分解 (Convex Decomposition):
--      - 凹多边形网格自动分解为多凸包复合体 (Compound Convex Hulls)
--      - 动态刚体 (Dynamic RigidBody3D) 真实物理碰撞解算
--   3. 交互控制:
--      - [1] 键: 镜头切换至贴花展区 (Decal Exhibition Zone)
--      - [2] 键: 镜头切换至凸包物理竞技场 (Convex Physics Arena)
--      - [Space] 键: 为动态凸包刚体施加向上跳跃冲量，观察高频碰撞解算
-- =============================================================================

local initialized = false
local cameraEntity = nil
local cameraTr = nil

local lastKey1 = false
local lastKey2 = false
local lastSpace = false

ecs:AddSystem("DecalConvexShowcaseSystem", "Update", function(dt: number)
    if not cameraEntity then
        local q = ecs:Query({ "TagComponent", "TransformComponent" })
        q:Each(function(e, tag, tr)
            if tag.Tag == "Decal Convex Camera" then
                cameraEntity = e
                cameraTr = tr
            end
        end)

        if not cameraEntity then
            return
        end

        if not initialized then
            initialized = true
            print("[SHOWCASE] ================================================================")
            print("[SHOWCASE] 欢迎进入 3D 投影贴花与凸包物理分解展示关卡 (DecalConvexShowcase)")
            print("[SHOWCASE] 核心特性 1: 3D 投影贴花 (Decal.slang / DecalRenderer)")
            print("[SHOWCASE]   - 观察墙面弹孔与地面血斑，支持角度掠射消隐 (Normal Cutoff)")
            print("[SHOWCASE] 核心特性 2: 真实 3D 凸包物理 (QuickHull / Compound Convex)")
            print("[SHOWCASE]   - 观察凹体高模分解为子凸包并参与 Jolt 动态刚体碰撞")
            print("[SHOWCASE] 交互快捷键:")
            print("[SHOWCASE]   [1] 观察贴花展区    [2] 观察凸包物理展区    [Space] 动态刚体跳跃")
            print("[SHOWCASE] ================================================================")
        end
    end

    -- [1] 贴花展区机位
    local k1 = Input.IsKeyPressed(49)
    if k1 and not lastKey1 and cameraTr then
        cameraTr.Location = vec3.new(-5.0, 3.2, 5.0)
        cameraTr.Rotation = quat.fromEuler(-10.0, -15.0, 0.0)
        print("[SHOWCASE] 相机已切换至: 3D 贴花投影展示区 (Decal Zone)")
    end
    lastKey1 = k1

    -- [2] 凸包物理展区机位
    local k2 = Input.IsKeyPressed(50)
    if k2 and not lastKey2 and cameraTr then
        cameraTr.Location = vec3.new(8.0, 4.5, 7.5)
        cameraTr.Rotation = quat.fromEuler(-16.0, 0.0, 0.0)
        print("[SHOWCASE] 相机已切换至: 凸包物理碰撞展示区 (Convex Physics Arena)")
    end
    lastKey2 = k2

    -- [Space] 刚体冲量与高度重置
    local sp = Input.IsKeyPressed(32)
    if sp and not lastSpace then
        local qPhysics = ecs:Query({ "ConvexCollider3DComponent", "TransformComponent" })
        qPhysics:Each(function(e, col, tr)
            local loc = tr.Location
            tr.Location = vec3.new(loc.x, loc.y + 3.0, loc.z)
            tr.Rotation = quat.fromEuler(25.0, 45.0, 0.0)
        end)
        print("[SHOWCASE] 已重置动态凸包碰撞体高度，观察自由落体与多面体弹跳碰撞!")
    end
    lastSpace = sp
end)

ecs:AddTeardownSystem("DecalConvexShowcaseTeardown", function()
    initialized = false
    cameraEntity = nil
    cameraTr = nil
end)
