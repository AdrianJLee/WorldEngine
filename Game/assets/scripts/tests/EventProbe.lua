-- P2 W4 headless fixture for the events/timers script binding.
-- The test host injects PROBE_EVENT(kind, ...) before the scene starts and observes
-- created / handles / order / payload / throwing / event-spawn / after / every /
-- cancel-target / timer-spawn / destroyed calls. Optional globals:
--   PROBE_MODE = "timer-spawn" | "timer-cancel" | anything else (default)
--   EMIT_PAYLOAD = true  -> the instance emits probe:payload once from OnUpdate
---@class EventProbe : WorldScript
local EventProbe = {}

local function report(...)
    if PROBE_EVENT then
        PROBE_EVENT(...)
    end
end

function EventProbe:OnCreate()
    local tag = self.entity:GetName()
    report("created", tag)

    self.handleOrder = events:on("probe:order", function(value)
        report("order", tag, value)
    end)
    self.handlePayload = events:on("probe:payload", function(number, flag, text, other)
        report("payload", tag, number, flag, text, other:GetName(), MARKER_SET == true)
    end)
    self.handleThrow = events:on("probe:throw", function()
        report("throwing", tag)
        -- 默认抛错;宿主可用 THROWING_TAG 只让指定实例抛错,验证"其它订阅者继续"。
        if THROWING_TAG == nil or THROWING_TAG == tag then
            error("probe handler exploded")
        end
    end)
    self.handleSpawn = events:on("probe:spawn", function()
        local child = self.entity:CreateChild("event child")
        child:AddComponent("TransformComponent")
        report("event-spawn", tag, child:IsValid(), self.entity:FindByName("event child") ~= nil)
    end)

    self.handleAfter = timers:after(0.5, function()
        report("after", tag)
    end)
    self.handleEvery = timers:every(0.25, function()
        report("every", tag)
    end, 2)

    if PROBE_MODE == "timer-spawn" then
        self.handleSpawnTimer = timers:after(2 / 60, function()
            local child = self.entity:CreateChild("timer child")
            child:AddComponent("TransformComponent")
            report("timer-spawn", tag, child:IsValid(), self.entity:FindByName("timer child") ~= nil)
        end)
    end
    if PROBE_MODE == "timer-cancel" then
        self.handleCancel = timers:every(0.1, function()
            report("cancel-target", tag)
        end)
    end

    report("handles", tag, self.handleOrder, self.handlePayload, self.handleThrow,
        self.handleSpawn, self.handleAfter, self.handleEvery,
        self.handleCancel or 0, self.handleSpawnTimer or 0)
end

---@param dt number
function EventProbe:OnUpdate(dt)
    if EMIT_PAYLOAD == true and self.emitted ~= true then
        self.emitted = true
        events:emit("probe:payload", 7, true, "hello", self.entity)
        -- 队列式 emit 的证据:handler 在本帧末看到这个标记为 true(同步投递时会是 nil)。
        MARKER_SET = true
        EMIT_PAYLOAD = nil
    end
end

function EventProbe:OnDestroy()
    report("destroyed", self.entity:GetName())
end

return EventProbe
