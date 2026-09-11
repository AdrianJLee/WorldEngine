-- Headless regression fixture. T02Record and T02Action are supplied by the test host.
---@class LifecycleProbe : WorldScript
---@field LocalUpdates integer
---@field Mode string
local LifecycleProbe = { LocalUpdates = 0, Mode = "normal" }

function LifecycleProbe:OnCreate()
    assert(self.entity:IsValid())
    assert(self.__Entity:GetID() == self.entity:GetID())
    assert(self.__EntityID:GetID() == self.entity:GetID())
    assert(self.entity:HasComponent("TagComponent"))
    T02Record(self.entity:GetID(), "create", 0)
    T02Action(self.entity, "create")
end

---@param dt number
function LifecycleProbe:OnUpdate(dt)
    assert(type(dt) == "number" and dt >= 0)
    self.LocalUpdates = self.LocalUpdates + 1
    T02Record(self.entity:GetID(), "update", self.LocalUpdates)
    if self.Mode == "destroy" then
        self.entity:Destroy()
        self.entity:Destroy()
    elseif self.Mode == "remove" then
        self.entity:RemoveComponent("LuaScriptComponent")
        self.entity:RemoveComponent("LuaScriptComponent")
    elseif self.Mode == "add" and self.LocalUpdates == 1 then
        self.entity:AddComponent("TransformComponent")
    end
    T02Action(self.entity, "update")
    -- A destroy/removal request must not truncate the callback that requested it.
    T02Record(self.entity:GetID(), "returned", self.LocalUpdates)
end

function LifecycleProbe:OnDestroy()
    -- Entity storage is still readable while lifecycle cleanup runs.
    assert(self.entity:IsValid() and self.entity:HasComponent("TagComponent"))
    T02Record(self.entity:GetID(), "destroy", self.LocalUpdates)
    T02Action(self.entity, "destroy")
end

if T02Inheritance then
    -- No lifecycle methods on the returned table: ordinary Lua inheritance must
    -- resolve all three through __index, while instance fields stay independent.
    return setmetatable({}, { __index = LifecycleProbe })
end

return LifecycleProbe
