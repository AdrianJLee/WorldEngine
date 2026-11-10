-- =============================================================================
-- scene_scope.luau — 演示脚本的关卡归属守卫
-- =============================================================================
-- 背景: <内容根>/scripts/systems 下的每个 .luau 都会被自动加载并注册进
--   **任何** 场景。演示脚本若不带守卫, 就会在别的关卡里也播放音频、抢输入、
--   驱动镜头, 表现为"换关卡后还有滋滋声 / 空格既跳跃又震屏"。
--
-- 用法(演示脚本顶部, 每个场景运行期只判定一次):
--   local SceneScope = ecs:RequireLib("scene_scope")
--   if not SceneScope.owns("Audio Demo Stage") then return end
--
-- 约定: 演示关卡里放一个带该 Tag 的实体作为"舞台标记"。脚本只在标记存在
--   的场景里生效, 其它场景静默退出。
-- =============================================================================

local SceneScope = {}

-- 当前场景是否含有指定 Tag 的实体(舞台标记)。
function SceneScope.hasTag(tagName: string): boolean
    if type(tagName) ~= "string" or tagName == "" then
        return false
    end

    local found = false
    local query = ecs:Query({ "TagComponent" })
    query:Each(function(_entity, tag)
        if not found and tag.Tag == tagName then
            found = true
        end
    end)
    return found
end

-- 便捷写法: 含标记即视为"本脚本拥有该场景"。
function SceneScope.owns(markerTag: string): boolean
    return SceneScope.hasTag(markerTag)
end

return SceneScope
