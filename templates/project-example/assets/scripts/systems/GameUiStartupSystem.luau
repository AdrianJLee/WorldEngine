-- =============================================================================
-- WorldEngine 游戏 UI 显式启动系统 (GameUiStartupSystem.luau)
-- 位于: templates/project-example/assets/scripts/systems/
-- 运行机制 (Pure ECS 架构):
--   1. 场景启动时由 ScriptEngine::LoadSystemScripts 自动加载并注册进帧管线;
--   2. 采用标准的 "Run-Once Startup System" 模式注册至 Update 阶段;
--   3. 首帧执行时主动调用 `ui.open("assets/ui/hud.wui")` 显式弹出游戏 HUD;
--   4. 执行完毕后调用 `ecs:RemoveSystem("GameUiStartupSystem")` 从管线注销自身,
--      零后续开销，彻底取代隐式黑盒自动附加。
-- =============================================================================

ecs:AddSystem("GameUiStartupSystem", "Update", function(dt: number)
    -- 1. 逻辑主动调用: 显式打开指定的游戏 UI 页面
    local ok = ui.open("assets/ui/hud.wui")
    if ok then
        print("[GameUI] 关卡启动系统: 已显式弹出游戏 HUD ('assets/ui/hud.wui')")
    else
        print("[GameUI] 关卡启动系统: 打开 HUD 失败，请确认文档路径")
    end

    -- 2. 仅执行 1 次: 完成后立即从场景帧管线中注销自身
    ecs:RemoveSystem("GameUiStartupSystem")
end)

print("[PureECS] GameUiStartupSystem 已注册 (首帧执行后自动注销)")
