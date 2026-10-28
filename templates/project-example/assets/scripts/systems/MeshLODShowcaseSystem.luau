-- =============================================================================
-- Mesh LOD 与 SceneOctree 空间八叉树综合展示系统 (MeshLODShowcaseSystem.luau)
-- =============================================================================
-- 演示 WorldEngine 网格 LOD 与空间八叉树核心技术:
--   1. 连续距离阶梯 LOD 评估: LOD0 (近景) -> LOD1 (中景) -> LOD2 (远景) -> 距离剔除 (Culled)
--   2. 滞后防抖阻尼 (Hysteresis Damping): 避免临界距离来回跳变闪烁
--   3. 空间八叉树 (SceneOctree) 视锥剔除与空间分区: 八叉树线框与实时遥测 HUD
-- =============================================================================

local initialized = false
local cameraEntity = nil
local cameraTr = nil
local nearPillarEntity = nil

-- 相机自动巡航控制
local autoPatrol = false
local patrolTime = 0.0
local patrolSpeed = 0.4
local patrolMinZ = -30.0
local patrolMaxZ = 30.0

-- 按键防抖
local lastSpace = false
local lastW = false
local lastS = false
local logTimer = 0.0

ecs:AddSystem("MeshLODShowcaseSystem", "Update", function(dt: number)
    -- 关卡作用域守卫：检查是否存在展示关卡特定实体
    if not cameraEntity or not nearPillarEntity then
        local q = ecs:Query({ "TagComponent", "TransformComponent" })
        q:Each(function(e, tag, tr)
            if tag.Tag == "Showcase Camera" then
                cameraEntity = e
                cameraTr = tr
            elseif tag.Tag == "LOD Pillar 01 (LOD 0 Near)" then
                nearPillarEntity = e
            end
        end)

        -- 非 MeshLODShowcase 关卡，静默退出
        if not cameraEntity or not nearPillarEntity then
            return
        end

        if not initialized then
            initialized = true
            print("[LOD-SHOWCASE] ════════════════════════════════════════════════════════════════")
            print("[LOD-SHOWCASE] Mesh LOD 与 SceneOctree 空间八叉树实机展示系统已激活")
            print("[LOD-SHOWCASE] 核心技术验证项:")
            print("[LOD-SHOWCASE]   1. 距离动态减面: LOD0(0~15m) -> LOD1(15~35m) -> LOD2(35~65m) -> 剔除(>65m)")
            print("[LOD-SHOWCASE]   2. 滞后防抖阻尼 (Hysteresis): 2.0m 缓冲区杜绝边界跳变")
            print("[LOD-SHOWCASE]   3. 空间八叉树 (SceneOctree): 空间 8 象限塔楼实体分区与视锥剔除")
            print("[LOD-SHOWCASE] 交互控制说明:")
            print("[LOD-SHOWCASE]   [Space 空格] -> 开启 / 暂停相机沿 Z 轴自动推拉巡航 (自动走过各 LOD 区间)")
            print("[LOD-SHOWCASE]   [W / S 键]   -> 手动拉近 / 拉远相机 Z 轴位置")
            print("[LOD-SHOWCASE] 视口调试开关 (编辑器 Viewport -> Show 菜单):")
            print("[LOD-SHOWCASE]   - Octree Bounds: 渲染八叉树层级 AABB 线框")
            print("[LOD-SHOWCASE]   - LOD Coloration: 全场景网格按 LOD 阶梯着色 (绿/黄/橙/红)")
            print("[LOD-SHOWCASE]   - LOD & Octree HUD: 视口右下角实时性能统计卡片")
            print("[LOD-SHOWCASE] ════════════════════════════════════════════════════════════════")
        end
    end

    logTimer = logTimer + dt

    -- 1. 空格键切换自动巡航
    local spaceDown = false
    if Input.IsKeyPressed then
        spaceDown = Input.IsKeyPressed(32) or Input.IsKeyPressed("Space")
    end
    if spaceDown and not lastSpace then
        autoPatrol = not autoPatrol
        print(string.format("[LOD-SHOWCASE] >>> 相机自动巡航模式: %s", autoPatrol and "开启 (推拉中)" or "暂停"))
    end
    lastSpace = spaceDown

    -- 2. W / S 键手动推进与拉退
    local manualZ = 0.0
    if Input.IsKeyPressed then
        if Input.IsKeyPressed(87) or Input.IsKeyPressed("W") or Input.IsKeyPressed("Up") then
            manualZ = manualZ - 12.0 * dt
        end
        if Input.IsKeyPressed(83) or Input.IsKeyPressed("S") or Input.IsKeyPressed("Down") then
            manualZ = manualZ + 12.0 * dt
        end
    end

    -- 3. 驱动相机移动
    if cameraTr then
        local loc = cameraTr.Location
        if autoPatrol then
            patrolTime = patrolTime + dt * patrolSpeed
            local wave = (math.sin(patrolTime) + 1.0) * 0.5 -- 0.0 ~ 1.0
            loc.z = patrolMinZ + wave * (patrolMaxZ - patrolMinZ)
            cameraTr.Location = loc
        elseif math.abs(manualZ) > 0.001 then
            loc.z = math.clamp(loc.z + manualZ, patrolMinZ - 10.0, patrolMaxZ + 10.0)
            cameraTr.Location = loc
        end
    end

    -- 4. 周期性状态回传
    if logTimer >= 3.0 then
        logTimer = 0.0
        if cameraTr then
            local camZ = cameraTr.Location.z
            local distNear = math.abs(camZ - 16.0)
            local currentStage = "LOD 0 (原始球体)"
            if distNear > 65.0 then
                currentStage = "Culled (距离剔除已隐藏)"
            elseif distNear > 35.0 then
                currentStage = "LOD 2 (远景档位)"
            elseif distNear > 15.0 then
                currentStage = "LOD 1 (中景档位)"
            end
            print(string.format("[LOD-SHOWCASE-TICK] 相机 Z=%.1f | 到 Near 实体距离=%.1fm | 当前所处区间: %s | 巡航: %s",
                camZ, distNear, currentStage, autoPatrol and "自动" or "手动"))
        end
    end
end)

ecs:AddTeardownSystem("MeshLODShowcaseTeardown", function()
    cameraEntity = nil
    cameraTr = nil
    nearPillarEntity = nil
    initialized = false
    autoPatrol = false
end)
