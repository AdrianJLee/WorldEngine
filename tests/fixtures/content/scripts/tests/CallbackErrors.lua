-- Intentional errors are selected by the headless test host.
if T02LoadMode == "load" then
    error("intentional load failure")
elseif T02LoadMode == "return" then
    return 42
elseif T02LoadMode == "callback" then
    return { OnCreate = "not a function" }
elseif T02LoadMode == "index" then
    return setmetatable({}, {
        __index = function()
            error("intentional inherited lookup failure")
        end,
    })
end

---@class CallbackErrors : WorldScript
---@field FailStage string
local CallbackErrors = { FailStage = "none" }

local function fail(stage)
    error("intentional " .. stage .. " failure")
end

function CallbackErrors:OnCreate()
    T02Record(self.entity:GetID(), "create", 0)
    if self.FailStage == "OnCreate" then fail("OnCreate") end
end

---@param dt number
function CallbackErrors:OnUpdate(dt)
    assert(type(dt) == "number")
    T02Record(self.entity:GetID(), "update", 0)
    if self.FailStage == "OnUpdate" then fail("OnUpdate") end
end

function CallbackErrors:OnDestroy()
    assert(self.entity:IsValid())
    T02Record(self.entity:GetID(), "destroy", 0)
    if self.FailStage == "OnDestroy" then fail("OnDestroy") end
end

return CallbackErrors
