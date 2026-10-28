-- =============================================================================
-- Mesh LOD 工业级综合展示关卡系统 (MeshLODShowcaseSystem.luau)
-- =============================================================================
-- 本系统演示 WorldEngine 核心 LOD 与空间八叉树技术:
--   1. 零配置约定式自动解析 (Zero-Config Auto LOD Resolution):
--      - 实体挂载 MeshLODComponent，即使槽位留空，渲染管线自动根据基准网格
--        匹配并加载 <stem>_LOD1.wmodel 与 <stem>_LOD2.wmodel
--   2. 多类别真实资产减面展示:
--      - 展区 A: 植被自然 (Scatter Pebble & Bush Cluster)
--      - 展区 B: 角色模型 (Character Mannequin)
--      - 展区 C: 硬表面机械与岩体 (D5 Fixture & Debris Shard)
--      - 展区 D: 标准几何图元 (Sphere & Cube)
--      - 展区 E: 远距剔除分界线 (Culling Boundary Marker)
--   3. 交互控制:
--      - [1 ~ 5] 键: 瞬间传送至五大展区最佳观察机位
--      - [Space] 键: 开启 / 暂停全展馆沿 Z 轴自动推拉平滑巡航
--      - [W / S] 键: 手动微调推进 / 拉远相机
-- =============================================================================

local initialized = false
local cameraEntity = nil
local cameraTr = nil

-- 相机自动巡航控制
local autoPatrol = false
local patrolTime = 0.0
local patrolSpeed = 0.35
local patrolMinZ = -50.0
local patrolMaxZ = 28.0

-- 按键防抖
local lastSpace = false
local lastKey1 = false
local lastKey2 = false
local lastKey3 = false
local lastKey4 = false
local lastKey5 = false
local logTimer = 0.0

ecs:AddSystem("MeshLODShowcaseSystem", "Update", function(dt: number)
    -- 关卡作用域守卫
    if not cameraEntity then
        local q = ecs:Query({ "TagComponent", "TransformComponent" })
        q:Each(function(e, tag, tr)
            if tag.Tag == "Showcase Camera" then
                cameraEntity = e
                cameraTr = tr
            end
        end)

        if not cameraEntity then
            return
        end

        if not initialized then
            initialized = true
            print("[LOD-SHOWCASE] ════════════════════════════════════════════════════════════════")
            print("[LOD-SHOWCASE] 欢迎进入 Mesh LOD 工业级综合展览馆 (MeshLODShowcase)")
            print("[LOD-SHOWCASE] 核心架构验证: 零配置自动解析 (Zero-Config Auto LOD Resolution)")
            print("[LOD-SHOWCASE]   场景内所有实体 MeshLODComponent 槽位均为空白，")
            print("[LOD-SHOWCASE]   引擎渲染管线 100% 自动解析并绑定同名 _LOD1 / _LOD2 模型资产！")
            print("[LOD-SHOWCASE] 展区导航与快捷传送:")
            print("[LOD-SHOWCASE]   [1 键] -> 传送至 展区 A (自然植被: 卵石岩体 & 灌木簇群, Z=26)")
            print("[LOD-SHOWCASE]   [2 键] -> 传送至 展区 B (角色模型: 人体模特 Mannequin, Z=10)")
            print("[LOD-SHOWCASE]   [3 键] -> 传送至 展区 C (机械碎石: D5机械治具 & 岩石破片, Z=-8)")
            print("[LOD-SHOWCASE]   [4 键] -> 传送至 展区 D (基础图元: 标准球体 & 立方体, Z=-26)")
            print("[LOD-SHOWCASE]   [5 键] -> 传送至 展区 E (剔除边界: 远距剔除警戒线, Z=-46)")
            print("[LOD-SHOWCASE]   [Space]  -> 开启 / 暂停全场自动平滑推拉巡航 (走完所有 LOD 梯度)")
            print("[LOD-SHOWCASE]   [W / S]  -> 手动拉近 / 拉远相机")
            print("[LOD-SHOWCASE] 视口调试开关 (编辑器 Viewport -> Show 菜单):")
            print("[LOD-SHOWCASE]   - Octree Bounds: 显示八叉树 3D 空间分区线框")
            print("[LOD-SHOWCASE]   - LOD Coloration: 绿(LOD0) / 黄(LOD1) / 橙(LOD2) / 红(Culled)")
            print("[LOD-SHOWCASE]   - LOD & Octree HUD: 视口右下角实时性能统计药丸")
            print("[LOD-SHOWCASE] ════════════════════════════════════════════════════════════════")
        end
    end

    logTimer = logTimer + dt

    -- 快捷键传送
    if Input.IsKeyPressed and cameraTr then
        -- 1 键 -> 展区 A (Z = 26)
        local k1 = Input.IsKeyPressed(49) or Input.IsKeyPressed("1")
        if k1 and not lastKey1 then
            autoPatrol = false
            cameraTr.Location = vec3.new(0.0, 3.5, 26.0)
            print("[LOD-SHOWCASE] >>> 已传送至 [展区 A - 自然植被展台 (卵石岩体 & 灌木丛)]")
        end
        lastKey1 = k1

        -- 2 键 -> 展区 B (Z = 10)
        local k2 = Input.IsKeyPressed(50) or Input.IsKeyPressed("2")
        if k2 and not lastKey2 then
            autoPatrol = false
            cameraTr.Location = vec3.new(0.0, 3.0, 10.0)
            print("[LOD-SHOWCASE] >>> 已传送至 [展区 B - 角色模型展台 (人体模特 Mannequin)]")
        end
        lastKey2 = k2

        -- 3 键 -> 展区 C (Z = -8)
        local k3 = Input.IsKeyPressed(51) or Input.IsKeyPressed("3")
        if k3 and not lastKey3 then
            autoPatrol = false
            cameraTr.Location = vec3.new(0.0, 3.2, -8.0)
            print("[LOD-SHOWCASE] >>> 已传送至 [展区 C - 机械与破片展台 (D5治具 & 岩体破片)]")
        end
        lastKey3 = k3

        -- 4 键 -> 展区 D (Z = -26)
        local k4 = Input.IsKeyPressed(52) or Input.IsKeyPressed("4")
        if k4 and not lastKey4 then
            autoPatrol = false
            cameraTr.Location = vec3.new(0.0, 3.2, -26.0)
            print("[LOD-SHOWCASE] >>> 已传送至 [展区 D - 基础几何图元展台 (标准球体 & 立方体)]")
        end
        lastKey4 = k4

        -- 5 键 -> 展区 E (Z = -46)
        local k5 = Input.IsKeyPressed(53) or Input.IsKeyPressed("5")
        if k5 and not lastKey5 then
            autoPatrol = false
            cameraTr.Location = vec3.new(0.0, 3.0, -46.0)
            print("[LOD-SHOWCASE] >>> 已传送至 [展区 E - 远距剔除警戒边界 (Cull Boundary)]")
        end
        lastKey5 = k5
    end

    -- 空格键切换自动巡航
    local spaceDown = false
    if Input.IsKeyPressed then
        spaceDown = Input.IsKeyPressed(32) or Input.IsKeyPressed("Space")
    end
    if spaceDown and not lastSpace then
        autoPatrol = not autoPatrol
        print(string.format("[LOD-SHOWCASE] >>> 全场自动巡航: %s", autoPatrol and "开启 (平滑推拉)" or "暂停"))
    end
    lastSpace = spaceDown

    -- W / S 手动微调
    local manualZ = 0.0
    if Input.IsKeyPressed then
        if Input.IsKeyPressed(87) or Input.IsKeyPressed("W") or Input.IsKeyPressed("Up") then
            manualZ = manualZ - 14.0 * dt
        end
        if Input.IsKeyPressed(83) or Input.IsKeyPressed("S") or Input.IsKeyPressed("Down") then
            manualZ = manualZ + 14.0 * dt
        end
    end

    -- 驱动相机
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

    -- 周期性状态播报
    if logTimer >= 3.0 then
        logTimer = 0.0
        if cameraTr then
            local camZ = cameraTr.Location.z
            local zoneName = "展区 A (植被自然)"
            local focalZ = 18.0
            if camZ < -42.0 then
                zoneName = "展区 E (剔除边界)"
                focalZ = -55.0
            elseif camZ < -24.0 then
                zoneName = "展区 D (标准图元)"
                focalZ = -34.0
            elseif camZ < -6.0 then
                zoneName = "展区 C (机械碎石)"
                focalZ = -16.0
            elseif camZ < 10.0 then
                zoneName = "展区 B (角色模型)"
                focalZ = 2.0
            end

            local dist = math.abs(camZ - focalZ)
            local currentStage = "LOD 0 (原模 100%)"
            if dist > 65.0 then
                currentStage = "Culled (距离剔除)"
            elseif dist > 35.0 then
                currentStage = "LOD 2 (25% 远模)"
            elseif dist > 15.0 then
                currentStage = "LOD 1 (50% 中模)"
            end

            print(string.format("[LOD-HUD] 相机 Z=%.1f | 当前聚焦: %s | 距离展台=%.1fm | 当前所处档位: %s | 自动巡航: %s",
                camZ, zoneName, dist, currentStage, autoPatrol and "开" or "关"))
        end
    end
end)

ecs:AddTeardownSystem("MeshLODShowcaseTeardown", function()
    cameraEntity = nil
    cameraTr = nil
    initialized = false
    autoPatrol = false
end)
