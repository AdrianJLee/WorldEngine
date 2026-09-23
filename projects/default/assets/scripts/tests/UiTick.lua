-- W3c/OnUI 快照探针(供 LuauUiBindingTests 使用):只记录 UI 阶段被调用的次数,
-- 用来断言"同一趟遍历里每个实例各跑一次"(快照不重复、不漏)。
local UiTick = {}

UiTick.uiTicks = 0

function UiTick:OnUI()
    self.uiTicks = self.uiTicks + 1
end

return UiTick
