---@class PlayerScript : WorldScript
---@field Speed number 移动速度
---@field Name string 显示名称
local PlayerScript = { Speed = 5.0, Name = "Player" }

function PlayerScript:OnCreate()
    print("Player initialized", self.Name, self.entity:GetID())
end

---@param dt number 距离上一帧的秒数
function PlayerScript:OnUpdate(dt)
    local direction = vec3.new(1.0, 0.0, 0.0)
    local distance = direction:length() * self.Speed * dt
    -- 在此编写行为；distance 的类型为 number。
end

function PlayerScript:OnDestroy()
    print("Player destroyed", self.entity:GetID())
end

return PlayerScript
