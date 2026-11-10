-- Deliberately wrong calls at the end are used only by the LuaLS acceptance check.
---@class CompletionProbe : WorldScript
---@field Speed number 移动速度
local Probe = { Speed = 5.0 }

---@param dt number 距离上一帧的秒数
function Probe:OnUpdate(dt)
    local direction = vec3.new(1.0, 0.0, 0.0)
    local length = direction:length()
    local id = self.entity:GetID()
    local distance = self.Speed * dt
    print(length, id, distance)

    self.entity:HasComponent(42) -- EXPECT param-type-mismatch
    self.entity:GetComponent(false) -- EXPECT param-type-mismatch
end

return Probe
