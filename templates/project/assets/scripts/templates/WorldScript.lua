-- 新建脚本模板(assets/scripts/templates/WorldScript.lua)。
-- 从 Scripts 面板的 "New Script" 复制到 assets/scripts/ 下,并修改类名与行为。
-- WorldScript 是类型注解,不是运行时构造器;API 提示来自生成的 WorldEngineAPI.luau。
---@class NewScript : WorldScript
---@field Speed number 移动速度
local NewScript = { Speed = 5.0 }

function NewScript:OnCreate()
    print("Created entity", self.entity:GetID())
end

---@param dt number 距离上一帧的秒数
function NewScript:OnUpdate(dt)
    local direction = vec3.new(1.0, 0.0, 0.0)
    local distance = direction:length() * self.Speed * dt
    -- 在此编写行为;让这个实体沿 +X 移动 distance 距离。
end

function NewScript:OnDestroy()
    print("Destroyed entity", self.entity:GetID())
end

return NewScript
