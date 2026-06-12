
-- Player.lua
local PlayerScript = {} -- 创建一个 Table 作为模块返回
PlayerScript.Speed = 5.0
PlayerScript.Name="12345"
---@type UUID
-- 这个会被缓存进 sc.OnCreateFunc
function PlayerScript:OnCreate()
    -- 利用 LuaEnv 沙盒的特性，我们可以安全地在这里定义变量
    -- 这些变量只会属于当前挂载了这个脚本的 Entity！

    print("Player Entity Initialized!")
    print("Name is: " .. self.Name)
    print("Speed is: " .. self.Speed)
    print("Speed is: "..self.__Entity:GetID())

end

-- 这个会被缓存进 sc.OnUpdateFunc
function PlayerScript:OnUpdate(ts)
    --local transform = entity:GetTransform()
    --transform.Location.y = transform.Location.y + PlayerScript.Speed * ts
    --transform:SetLocation(transform.Location)
end

-- 这个会被缓存进 sc.OnDestroyFunc
function PlayerScript:OnDestroy()
    print("Player Entity Destroyed!")
end

-- 必须 return 这个 table，否则 C++ 端的 auto result = script_file(...) 拿不到数据！
return PlayerScript