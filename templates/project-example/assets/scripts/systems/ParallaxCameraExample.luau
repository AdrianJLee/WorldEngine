-- =============================================================================
-- 2D 游戏全特性综合演示系统 (ParallaxCameraExample.luau)
-- =============================================================================
-- 本系统演示 WorldEngine 2D 核心特性闭环:
--   1. PixelPerfectCameraComponent: 目标参考分辨率与 PPU 整数倍缩放评估
--   2. ParallaxLayerComponent: 视差多景深层根据相机位移自动做平滑背景滚动
--   3. PlatformEffector2DComponent: 单向跳跃平台效应器 (自下而上穿透, 顶部站立)
--   4. PolygonCollider2D / ChainCollider2D: 凹凸多边形与连续地形轮廓链条物理
--   5. TilemapComponent: 正交 / 等轴测 2.5D (Isometric) / 六边形蜂窝 (Hexagonal) 瓦片地图
--   6. 2D 光照与阴影: GlobalLight2D / PointLight2D / ShadowCaster2D 动态光源与阴影投射
--   7. 2D 物理关节: DistanceJoint2D / RevoluteJoint2D / PrismaticJoint2D / WheelJoint2D
--   8. 2D 物理效应器: AreaEffector2D (风场/引力) / BuoyancyEffector2D (水体浮力) / SurfaceEffector2D (传送带)
-- =============================================================================

local initialized = false

local gridTypeNames = {
    [0] = "Orthogonal (正交直角)",
    [1] = "Isometric (等轴测 2.5D)",
    [2] = "HexagonalPointy (尖顶六边形)",
    [3] = "HexagonalFlat (平顶六边形)"
}

ecs:AddSystem("ParallaxCameraDemoSystem", "Update", function(dt: number)
    if not initialized then
        initialized = true
        print("[2D-DEMO] 2D 全特性演示系统启动")

        -- 1) 查询像素相机状态
        local camQuery = ecs:Query({ "PixelPerfectCameraComponent", "CameraComponent" })
        camQuery:Each(function(e, ppc, cam)
            print(string.format("[2D-DEMO] 像素相机: PPU=%d | 参考分辨率=(%d, %d) | 当前缩放=%dx",
                ppc.PixelsPerUnit, ppc.ReferenceResolution.x, ppc.ReferenceResolution.y, ppc.CurrentScale))
        end)

        -- 2) 查询视差背景层
        local plxQuery = ecs:Query({ "ParallaxLayerComponent", "TagComponent" })
        plxQuery:Each(function(e, plx, tag)
            print(string.format("[2D-DEMO] 视差层 '%s': ScrollFactor=(%.2f, %.2f) | RepeatSize=(%.1f, %.1f)",
                tag.Tag, plx.ScrollFactor.x, plx.ScrollFactor.y, plx.RepeatSize.x, plx.RepeatSize.y))
        end)

        -- 3) 查询单向跳跃平台效应器
        local platQuery = ecs:Query({ "PlatformEffector2DComponent", "TagComponent" })
        platQuery:Each(function(e, eff, tag)
            print(string.format("[2D-DEMO] 单向平台 '%s': OneWay=%s | 表面弧度=%.1f°",
                tag.Tag, tostring(eff.OneWay), eff.SurfaceArc))
        end)

        -- 4) 查询多边形与地形链条碰撞体
        local polyQuery = ecs:Query({ "PolygonCollider2DComponent", "TagComponent" })
        polyQuery:Each(function(e, poly, tag)
            print(string.format("[2D-DEMO] 多边形碰撞体 '%s': 顶点串='%s'", tag.Tag, poly.Points))
        end)

        local chainQuery = ecs:Query({ "ChainCollider2DComponent", "TagComponent" })
        chainQuery:Each(function(e, chain, tag)
            print(string.format("[2D-DEMO] 地形链条 '%s': 轮廓串='%s' | 闭合=%s",
                tag.Tag, chain.Points, tostring(chain.Loop)))
        end)

        -- 5) 查询高级拓扑瓦片地图 (正交 / 等轴测 2.5D / 六边形)
        local tmQuery = ecs:Query({ "TilemapComponent", "TagComponent" })
        tmQuery:Each(function(e, tm, tag)
            local tName = gridTypeNames[tm.GridType] or tostring(tm.GridType)
            print(string.format("[2D-DEMO] 瓦片地图 '%s': 拓扑=%s | 资产='%s' | 网格尺寸=(%.2f, %.2f)",
                tag.Tag, tName, tm.Tilemap, tm.CellSize.x, tm.CellSize.y))
        end)

        -- 6) Phase 9: 查询 2D 光源与阴影投射器
        local glQuery = ecs:Query({ "GlobalLight2DComponent", "TagComponent" })
        glQuery:Each(function(e, gl, tag)
            print(string.format("[2D-DEMO] 2D 全局氛围光 '%s': 强度=%.2f | 方向凹凸=%.2f",
                tag.Tag, gl.Intensity, gl.DirectionIntensity))
        end)

        local plQuery = ecs:Query({ "PointLight2DComponent", "TagComponent" })
        plQuery:Each(function(e, pl, tag)
            print(string.format("[2D-DEMO] 2D 点光源 '%s': 颜色=(%.2f, %.2f, %.2f) | 强度=%.2f | 外径=%.1f | 阴影=%s",
                tag.Tag, pl.Color.x, pl.Color.y, pl.Color.z, pl.Intensity, pl.OuterRadius, tostring(pl.CastShadows)))
        end)

        local casterQuery = ecs:Query({ "ShadowCaster2DComponent", "TagComponent" })
        casterQuery:Each(function(e, caster, tag)
            print(string.format("[2D-DEMO] 2D 阴影投射器 '%s': 尺寸=(%.1f, %.1f) | 挤出长度=%.1f | 阴影浓度=%.2f",
                tag.Tag, caster.Size.x, caster.Size.y, caster.ExtrusionLength, caster.ShadowIntensity))
        end)

        -- 7) Phase 10: 查询 2D 物理关节约束 (Distance / Revolute / Prismatic / Wheel)
        local distJointQuery = ecs:Query({ "DistanceJoint2DComponent", "TagComponent" })
        distJointQuery:Each(function(e, dist, tag)
            print(string.format("[2D-DEMO] 2D 距离关节 '%s': 目标实体ID=%s | 弹簧=%s (%.1f Hz) | 距离限制=%s (%.1f~%.1f)",
                tag.Tag, tostring(dist.ConnectedEntity), tostring(dist.EnableSpring), dist.Hertz,
                tostring(dist.EnableLimit), dist.MinDistance, dist.MaxDistance))
        end)

        local revJointQuery = ecs:Query({ "RevoluteJoint2DComponent", "TagComponent" })
        revJointQuery:Each(function(e, rev, tag)
            print(string.format("[2D-DEMO] 2D 旋转关节(铰链) '%s': 目标实体ID=%s | 锚点=(%.2f, %.2f) | 电机=%s (%.1f deg/s) | 限制=%s (%.1f°~%.1f°)",
                tag.Tag, tostring(rev.ConnectedEntity), rev.AnchorSelf.x, rev.AnchorSelf.y,
                tostring(rev.EnableMotor), rev.MotorSpeed, tostring(rev.EnableLimit), rev.LowerAngle, rev.UpperAngle))
        end)

        local prismJointQuery = ecs:Query({ "PrismaticJoint2DComponent", "TagComponent" })
        prismJointQuery:Each(function(e, prism, tag)
            print(string.format("[2D-DEMO] 2D 移动关节(滑块) '%s': 目标实体ID=%s | 滑动轴=(%.2f, %.2f) | 电机=%s (%.1f m/s) | 限制=%s (%.1f~%.1f m)",
                tag.Tag, tostring(prism.ConnectedEntity), prism.Axis.x, prism.Axis.y,
                tostring(prism.EnableMotor), prism.MotorSpeed, tostring(prism.EnableLimit), prism.LowerTranslation, prism.UpperTranslation))
        end)

        local wheelJointQuery = ecs:Query({ "WheelJoint2DComponent", "TagComponent" })
        wheelJointQuery:Each(function(e, wheel, tag)
            print(string.format("[2D-DEMO] 2D 车轮关节(悬挂) '%s': 目标实体ID=%s | 悬挂轴=(%.2f, %.2f) | 弹簧阻尼=%.1f Hz / %.2f | 行程限制=%s (%.2f~%.2f m)",
                tag.Tag, tostring(wheel.ConnectedEntity), wheel.SuspensionAxis.x, wheel.SuspensionAxis.y,
                wheel.Hertz, wheel.DampingRatio, tostring(wheel.EnableLimit), wheel.LowerTranslation, wheel.UpperTranslation))
        end)

        -- 8) Phase 11: 查询 2D 物理效应器 (AreaEffector2D / BuoyancyEffector2D / SurfaceEffector2D)
        local areaQuery = ecs:Query({ "AreaEffector2DComponent", "TagComponent" })
        areaQuery:Each(function(e, area, tag)
            print(string.format("[2D-DEMO] 2D 区域效应器(风场/引力) '%s': 模式=%d | 力量=%.1f N | 方向=(%.2f, %.2f) | 阻尼=%.2f",
                tag.Tag, area.ForceMode, area.ForceMagnitude, area.ForceDirection.x, area.ForceDirection.y, area.LinearDrag))
        end)

        local buoyQuery = ecs:Query({ "BuoyancyEffector2DComponent", "TagComponent" })
        buoyQuery:Each(function(e, buoy, tag)
            print(string.format("[2D-DEMO] 2D 浮力效应器(水体) '%s': 密度=%.2f | 水面高度=%.2f | 水流速度=(%.2f, %.2f) | 阻尼=%.2f",
                tag.Tag, buoy.Density, buoy.SurfaceLevel, buoy.FlowVelocity.x, buoy.FlowVelocity.y, buoy.LinearDrag))
        end)

        local surfQuery = ecs:Query({ "SurfaceEffector2DComponent", "TagComponent" })
        surfQuery:Each(function(e, surf, tag)
            print(string.format("[2D-DEMO] 2D 表面效应器(传送带) '%s': 切线速度=%.2f m/s | 摩擦推力倍率=%.2f | 启用=%s",
                tag.Tag, surf.Speed, surf.ForceScale, tostring(surf.Enabled)))
        end)
    end
end)

print("[2D-DEMO] ParallaxCameraDemoSystem 已注册入 ECS")
-- =========================================================================
-- 动画关键帧事件监听示例 (Animation Keyframe Events Listener)
-- =========================================================================
-- 当角色播放带有 EventTag 的动画关键帧 (如 hero_walk.wanim 中的 step_left / step_right) 时
-- 引擎将精确打点并在当前帧通过 ecs:OnAnimationEvent 派发到脚本
local animEventHandle = ecs:OnAnimationEvent(function(event)
    local entityName = "Unknown"
    if event.entity and event.entity:IsValid() then
        if event.entity:HasComponent("TagComponent") then
            local ok, name = pcall(function() return event.entity:GetName() end)
            if ok and name and #name > 0 then
                entityName = name
            else
                local tagComp = event.entity:Get("TagComponent")
                if tagComp and tagComp.Tag then
                    entityName = tagComp.Tag
                end
            end
        else
            entityName = "Entity#" .. tostring(event.entity:GetID())
        end
    end
    print(string.format("[ANIM-EVENT] 实体 '%s' 触发动画事件: Tag='%s' | Frame=%d",
        entityName, event.tag, event.frame))

    if event.tag == "step_left" or event.tag == "step_right" then
        print(string.format("  -> 触发移动脚步音效逻辑: assets/audio/sfx/footstep_grass.waudio (Tag: %s)", event.tag))
    elseif event.tag == "attack_hit" then
        print(string.format("  -> 开启攻击判定伤害盒 (HitBox Opened)"))
    end
end)

print("[2D-DEMO] 动画关键帧事件监听器已注册 (Handle=" .. tostring(animEventHandle) .. ")")

ecs:AddTeardownSystem("ParallaxCameraDemoTeardown", function()
    initialized = false
end)
