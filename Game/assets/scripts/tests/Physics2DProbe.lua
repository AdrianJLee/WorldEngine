-- W3f headless fixture: drive a 2D rigid body through the Entity runtime API.
-- The C++ harness selects behaviour through the TEST_Physics2DMode global and
-- reports observations through the injected TEST_Physics2DReport function.
---@class Physics2DProbe : WorldScript
local Physics2DProbe = {}

function Physics2DProbe:OnCreate()
    assert(self.entity:IsValid())
end

---@param dt number
function Physics2DProbe:OnUpdate(dt)
    local mode = TEST_Physics2DMode
    local entity = self.entity
    if mode == "drive" then
        local position = entity:GetComponent("TransformComponent").Location
        local velocity = entity:GetLinearVelocity()
        if TEST_Physics2DPhase == 1 then
            TEST_Physics2DReport("start", position.x, position.y, velocity.x, velocity.y)
        else
            TEST_Physics2DReport("end", position.x, position.y, velocity.x, velocity.y)
        end
    elseif mode == "angular" then
        entity:SetAngularVelocity(2.0)
        assert(math.abs(entity:GetAngularVelocity() - 2.0) < 0.001)
    elseif mode == "force" then
        if TEST_Physics2DPhase == 4 then
            entity:SetLinearVelocity(vec2.new(0.0, 0.0))
            -- 10 N 向上 + 重力 / 1 kg：恰好抵消加速度，落回 0 m/s。
            entity:ApplyForce(vec2.new(0.0, 10.0))
        end
    elseif mode == "impulse" then
        if TEST_Physics2DPhase == 2 then
            entity:SetLinearVelocity(vec2.new(0.0, 0.0))
            entity:ApplyLinearImpulse(vec2.new(1.0, 0.0))
        end
    elseif mode == "sync" then
        local transform = entity:GetComponent("TransformComponent")
        transform.Location = vec3.new(7.0, 11.0, 0.0)
        entity:SyncPhysicsBody()
    elseif mode == "add_body" then
        if TEST_Physics2DPhase == 1 then
            -- 先加组件并把类型置为 Dynamic,再加碰撞体:shape 直接建在动态刚体上(质量一次成型)。
            entity:AddComponent("RigidBody2DComponent")
            local body = entity:GetComponent("RigidBody2DComponent")
            assert(body ~= nil)
            body.Type = "Dynamic"
            entity:AddComponent("BoxCollider2DComponent")
            assert(entity:HasComponent("BoxCollider2DComponent"))
            entity:SetLinearVelocity(vec2.new(0.0, 1.0))
            local position = entity:GetComponent("TransformComponent").Location
            TEST_Physics2DReport("added", position.x, position.y, 0.0, 1.0)
        else
            local position = entity:GetComponent("TransformComponent").Location
            TEST_Physics2DReport("moved", position.x, position.y, 0.0, 1.0)
        end
    elseif mode == "remove_body" then
        entity:RemoveComponent("RigidBody2DComponent")
    elseif mode == "no_body" then
        local ok, err = pcall(function() return entity:GetLinearVelocity() end)
        assert(not ok, "an entity without a RigidBody2DComponent must raise")
        assert(type(err) == "string")
        assert(string.find(err, "requires a 2D rigid body", 1, true), err)
        assert(string.find(err, "GetLinearVelocity", 1, true), err)
    elseif mode == "apply_no_body" then
        local ok, err = pcall(function() entity:ApplyLinearImpulse(vec2.new(1.0, 0.0)) end)
        assert(not ok, "impulse without a body must raise")
        assert(string.find(err, "requires a 2D rigid body", 1, true), err)
        local ok2, err2 = pcall(function() entity:SetAngularVelocity(1.0) end)
        assert(not ok2, "angular velocity without a body must raise")
        assert(string.find(err2, "requires a 2D rigid body", 1, true), err2)
    end
end

return Physics2DProbe
