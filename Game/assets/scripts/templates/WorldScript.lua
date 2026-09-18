-- 复制到 scripts 下，并修改类名。WorldScript 是类型注解，不是运行时构造器。
---@class ExampleScript : WorldScript
---@field Speed number 移动速度
local ExampleScript = { Speed = 5.0 }

function ExampleScript:OnCreate()
    print("Created entity", self.entity:GetID())
end

---@param dt number 距离上一帧的秒数
function ExampleScript:OnUpdate(dt)
    local direction = vec3.new(1.0, 0.0, 0.0)
    local distance = direction:length() * self.Speed * dt
    -- 在此编写行为；API 提示来自生成的 WorldEngineAPI.luau（见 .luau-lsp 配置）。
end

function ExampleScript:OnDestroy()
    print("Destroyed entity", self.entity:GetID())
end

return ExampleScript
