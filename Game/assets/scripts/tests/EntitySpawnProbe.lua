-- W3d headless fixture: exercises the synchronous Entity API from OnUpdate.
--   spawn  -> create child "A", add Transform+Sprite, write Location, read it back
--   query  -> assert same-frame creation is invisible to FindByName (snapshot semantics)
--   prefab -> instantiate PrefabPath and report the root through TEST_CaptureEntity
---@class EntitySpawnProbe : WorldScript
---@field Mode string
---@field PrefabPath string
local EntitySpawnProbe = { Mode = "none", PrefabPath = "" }

function EntitySpawnProbe:OnCreate()
    assert(self.entity:IsValid())
    assert(self.entity:GetName() ~= "")
end

---@param dt number
function EntitySpawnProbe:OnUpdate(dt)
    assert(type(dt) == "number")
    if self.Mode == "spawn" and not self.Spawned then
        self.Spawned = true
        local child = self.entity:CreateChild("A")
        assert(child:IsValid())
        assert(child:GetName() == "A")
        assert(child:GetParent():GetID() == self.entity:GetID())
        child:AddComponent("TransformComponent")
        child:AddComponent("SpriteComponent")
        local transform = child:GetComponent("TransformComponent")
        assert(transform ~= nil)
        transform.Location = vec3.new(1.0, 2.0, 3.0)
        local read = child:GetComponent("TransformComponent")
        assert(read ~= nil)
        assert(read.Location.x == 1.0 and read.Location.y == 2.0 and read.Location.z == 3.0)
        assert(child:HasComponent("SpriteComponent"))
    elseif self.Mode == "query" then
        -- The spawner ran earlier in the same update snapshot: the new entity stays hidden.
        assert(self.entity:FindByName("A") == nil)
    elseif self.Mode == "prefab" and not self.PrefabDone then
        self.PrefabDone = true
        local root = self.entity:InstantiatePrefab(self.PrefabPath)
        assert(root:IsValid())
        TEST_CaptureEntity(root)
    elseif self.Mode == "destroy" and not self.DestroyRequested then
        self.DestroyRequested = true
        local doomed = self.entity:CreateChild("Doomed")
        doomed:Destroy()
        -- Destroy is still deferred: the handle remains valid inside the callback.
        assert(doomed:IsValid())
    elseif self.Mode == "remove" and not self.RemoveRequested then
        self.RemoveRequested = true
        self.entity:AddComponent("TransformComponent")
        self.entity:RemoveComponent("TransformComponent")
        -- RemoveComponent is still deferred: the component is visible until the commit point.
        assert(self.entity:HasComponent("TransformComponent"))
    end
end

return EntitySpawnProbe
