-- =============================================================================
-- WorldEngine 游戏 UI 显式启动系统 (GameUiStartupSystem.luau)
-- 位于: templates/project-example/assets/scripts/systems/
-- 机制:
--   使用引擎专用的原生生命周期 API `ecs:AddStartupSystem` 与 `ecs:AddTeardownSystem`;
--   - StartupSystem: 场景进入运行态 (OnRuntimeStart) 时由引擎内核执行且仅执行 1 次;
--   - TeardownSystem: 场景停止时 (StopScene) 执行 1 次做清理收尾;
--   原生单次执行，不占用 Update 帧管线，无需手动注销。
-- =============================================================================

local SceneScope = nil
local okScope, scopeLib = pcall(function() return ecs:RequireLib("scene_scope") end)
if okScope and type(scopeLib) == "table" then
    SceneScope = scopeLib
end

-- 1. 场景开始时执行一次的原生启动系统 (仅在拥有 HUD Demo 标记的关卡生效)
ecs:AddStartupSystem("GameUiStartupSystem", function()
    local inScope = false
    if SceneScope and type(SceneScope.owns) == "function" then
        inScope = SceneScope.owns("HUD Demo Stage") or SceneScope.owns("Input Demo Stage")
    else
        local q = ecs:Query({ "TagComponent" })
        q:Each(function(_e, tag)
            if tag.Tag == "HUD Demo Stage" or tag.Tag == "Input Demo Stage" then
                inScope = true
            end
        end)
    end
    if not inScope then
        return
    end
    local ok = ui.open("assets/ui/hud.wui")
    if ok then
        print("[GameUI] 原生场景启动系统 (StartupSystem): 已显式弹出游戏 HUD ('assets/ui/hud.wui')")
    else
        print("[GameUI] 原生场景启动系统 (StartupSystem): 打开 HUD 失败，请确认文档路径")
    end
end)

-- 2. 场景停止时执行一次的原生收尾系统
ecs:AddTeardownSystem("GameUiTeardownSystem", function()
    if ui.isOpen() then
        ui.close()
        print("[GameUI] 原生场景收尾系统 (TeardownSystem): 已关闭清理 HUD")
    end
end)

print("[PureECS] GameUiStartupSystem 与 TeardownSystem 已注册至场景生命周期")
