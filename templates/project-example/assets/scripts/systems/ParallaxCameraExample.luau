-- =============================================================================
-- 2D 像素相机与视差背景演示系统 (ParallaxCameraExample.luau)
-- =============================================================================
-- 本系统演示 WorldEngine 2D 特性闭环:
--   1. PixelPerfectCameraComponent: 目标参考分辨率与 PPU 整数倍缩放评估
--   2. ParallaxLayerComponent: 视差多景深层根据相机位移自动做平滑背景滚动
-- =============================================================================

local initialized = false

ecs:AddSystem("ParallaxCameraDemoSystem", "Update", function(dt: number)
    if not initialized then
        initialized = true
        print("[2D-DEMO] 2D 像素相机与视差背景系统启动")

        -- 1) 查询像素相机状态
        local camQuery = ecs:Query({ "PixelPerfectCameraComponent", "CameraComponent" })
        camQuery:Each(function(e, ppc, cam)
            print(string.format("[2D-DEMO] 发现像素相机: PPU=%d | 参考分辨率=(%d, %d) | 当前缩放=%dx",
                ppc.PixelsPerUnit, ppc.ReferenceResolution.x, ppc.ReferenceResolution.y, ppc.CurrentScale))
        end)

        -- 2) 查询并打印场景中挂载的所有视差背景层
        local plxQuery = ecs:Query({ "ParallaxLayerComponent", "TagComponent" })
        plxQuery:Each(function(e, plx, tag)
            print(string.format("[2D-DEMO] 视差层 '%s': ScrollFactor=(%.2f, %.2f) | RepeatSize=(%.1f, %.1f)",
                tag.Tag, plx.ScrollFactor.x, plx.ScrollFactor.y, plx.RepeatSize.x, plx.RepeatSize.y))
        end)
    end
end)

print("[2D-DEMO] ParallaxCameraDemoSystem 已注册入 ECS")
