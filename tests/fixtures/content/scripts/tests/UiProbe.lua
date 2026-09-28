-- P2 W3c headless fixture for the immediate-mode `ui` script table.
-- The test host drives one WuiContext frame per call to ScriptEngine::DrawScriptUi
-- and reads the script fields back through LuauScriptComponent::ScriptTable.
---@class UiProbe : WorldScript
---@field clicks integer
---@field checked boolean
---@field sliderValue number
---@field listIndex integer
---@field gridIndex integer
---@field failOnUi boolean
---@field failOnImage boolean
---@field rowsCount integer
---@field rowsW number
---@field rowsH number
---@field rowsX2 number
---@field columnsCount integer
---@field columnsH number
---@field columnsY2 number
---@field savedClicks integer
local UiProbe = {}

UiProbe.clicks = 0
UiProbe.checked = false
UiProbe.sliderValue = 0.5
UiProbe.listIndex = 1
UiProbe.gridIndex = 1
UiProbe.failOnUi = false
UiProbe.failOnImage = false
UiProbe.rowsCount = 0
UiProbe.rowsW = 0
UiProbe.rowsH = 0
UiProbe.rowsX2 = 0
UiProbe.columnsCount = 0
UiProbe.columnsH = 0
UiProbe.columnsY2 = 0
UiProbe.savedClicks = -1

function UiProbe:OnCreate()
    self.clicks = 0
    self.checked = false
    self.sliderValue = 0.5
    self.listIndex = 1
    self.gridIndex = 1
    self.failOnImage = false
    self.rowsCount = 0
    self.columnsCount = 0
    self.savedClicks = -1
end

function UiProbe:OnUI()
    -- ui.image 需要渲染设备;headless 下必须是可读 Lua error,不能触发 GL 断言。
    if self.failOnImage then
        ui.image(0, 0, 10, 10, "textures/Icon.wtex")
    end
    -- Error-isolation mode: an empty widget id is rejected by the binding with a
    -- readable Lua error and must only fault this instance.
    if self.failOnUi then
        ui.button("", 0, 0, 10, 10, "bad")
    end

    -- W3c 宿主接线:两个标记的屏幕位置是自动化像素断言的锚点
    -- (面板底色 (31,32,33) 在 (40..260, 150..190) 内;标记按钮底色 (51,54,59) 在 (981..1187, 481..517) 内)。
    ui.panel(10, 10, 300, 200, "HUD")
    ui.button("marker", 980, 480, 210, 40, "SCRIPT-UI")
    ui.text(20, 40, "Hello", 16)
    if ui.button("go", 20, 60, 100, 24, "Go") then
        self.clicks = self.clicks + 1
    end
    self.checked = ui.checkbox("check", 20, 90, 120, 20, "Enabled", self.checked)
    self.sliderValue = ui.slider("slide", 20, 116, 120, 20, self.sliderValue, 0, 10)

    local listHit = ui.list("list", 140, 60, 120, 72, { "alpha", "beta", "gamma" }, 24, self.listIndex)
    if listHit then
        self.listIndex = listHit
    end
    local gridHit = ui.grid("grid", 140, 140, 120, 50, { "one", "two" }, 60, 50, self.gridIndex)
    if gridHit then
        self.gridIndex = gridHit
    end

    -- Layout helpers return {x, y, w, h} tables; record the values the host asserts on.
    local rows = ui.rows(330, 10, 200, 62, 3, 4)
    self.rowsCount = #rows
    if rows[1] then
        self.rowsW = rows[1].w
        self.rowsH = rows[1].h
        self.rowsX2 = rows[2].x
    end
    local columns = ui.columns(330, 90, 100, 62, 3, 4)
    self.columnsCount = #columns
    if columns[1] then
        self.columnsH = columns[1].h
        self.columnsY2 = columns[2].y
    end

    -- W3c 宿主接线:把脚本状态变成宿主/自动化可观测的副作用。
    --   WLD_SAVE_DIR=<dir> 时 Save.Save(0) 落盘 <dir>/saves/slot-0.wsave,其中的
    --   Globals["ui.clicks"] 就是"注入的点击真的驱动了脚本 UI 回调"的证据。
    -- 沙箱里 os/io 都不可用,不能靠环境变量开开关;这里的判据是"服务能不能真的落盘":
    -- headless 夹具没有 GameApp 会话,Save.Save 会抛 Lua error,pcall 吃掉后保持 -1
    -- (不误报成功、也不能把 OnUI 打成 Faulted);宿主里成功才推进 savedClicks。
    if self.clicks ~= self.savedClicks then
        local clicks = self.clicks
        local ok, saved = pcall(function()
            Save.SetGlobal("ui.clicks", clicks)
            return Save.Save(0)
        end)
        if ok == true and saved == true then
            self.savedClicks = clicks
        end
    end
end

return UiProbe
