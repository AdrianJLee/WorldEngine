-- P2 W3b headless fixture for Input/Level/Save service bindings.
-- The test host injects PROBE_MOVE(axis, z) to observe the script-driven movement.
---@class ServicesProbe : WorldScript
local ServicesProbe = {}

function ServicesProbe:OnCreate()
end

---@param dt number
function ServicesProbe:OnUpdate(dt)
    -- 边沿读数:测试宿主按帧读取,按住跨帧时 Pressed 必须在第二帧变 false。
    PRESSED = Input.Pressed("Jump")
    DOWN = Input.Down("Jump")
    RELEASED = Input.Released("Jump")
    PLAYER_COUNT = Input.PlayerCount()
    local axis = Input.Axis("Move")
    AXIS = axis
    if PROBE_STATE then
        PROBE_STATE(PRESSED, DOWN, RELEASED,
            Input.Down("Ghost"), Input.Axis("Ghost"), Input.Down("Jump", 7), Input.Axis("Move", 7))
    end
    if axis ~= 0 then
        -- Axis 驱动移动:写 TransformComponent.Location,并把新 z 回调给测试宿主。
        local entity = self.entity
        local transform = entity:GetComponent("TransformComponent")
        local location = transform.Location
        local moved = vec3.new(location.x, location.y, location.z + axis * dt)
        transform.Location = moved
        if PROBE_MOVE then
            PROBE_MOVE(axis, moved.z)
        end
    end
end

return ServicesProbe
