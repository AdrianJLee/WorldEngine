-- W3c/OnUI 结构写探针(供 LuauUiBindingTests 使用):UI 阶段同步建子实体 + 挂纯数据组件,
-- 并当场验证"结构写是同帧可见的"(FindByName 立刻找得到)。
local UiSpawn = {}

UiSpawn.uiTicks = 0
UiSpawn.spawned = 0
UiSpawn.childVisible = false

function UiSpawn:OnUI()
    self.uiTicks = self.uiTicks + 1
    if self.spawned == 0 then
        local child = self.entity:CreateChild("OnUi Child")
        child:AddComponent("TransformComponent")
        self.spawned = 1
        self.childVisible = self.entity:FindByName("OnUi Child") ~= nil
    end
end

return UiSpawn
