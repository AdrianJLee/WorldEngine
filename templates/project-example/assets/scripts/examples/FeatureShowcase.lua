-- FeatureShowcase.lua
-- WorldEngine 脚本系统特性全景展示示例。
-- 演示脚本声明、生命周期、实体驱动、即时模式 UI、定时器系统以及属性面板交互。
-- 属性面板部分包含两类字段：
--   * 叶子字段(number/string/boolean/vec3…)：一行一个控件；
--   * 结构化表(---@class)：属性面板里可展开成子行、逐字段编辑、随场景保存；
--     裸 `table`(没有子字段声明)只做只读展示，不进存档。
-- 集合字段(数组/映射)的声明写法：
--   * 数组 `---@field Scores {number}`：每个元素一行（行标签 = 下标），行尾 `-` 删除、底部 `+` 追加；
--   * 映射 `---@field Config {string: number}`：每个键一行（行标签 = 键名），`+` 会先让你输入键名；
--   * 嵌套数组 `---@field Grid {{number}}`：元素本身还是数组（行里再展开一层）；
--   * 初值 = **默认形状**：没被编辑过的元素显示脚本里的初值、不写进场景；
--     编辑过/增删过的形状以场景为准（重开场景后仍是你的元素个数与键名）。
-- 复位（行尾 `↺`）= 回到“未设”：该字段从场景里消失，重新按脚本里的初值显示。

-- 结构化表的字段声明：属性面板会按这份声明展开子行（顺序 = 这里的顺序）。
---@class FeatureShowcaseStats
---@field Damage number 攻击力
---@field Range number 射程（米）
---@field Label string 数值面板标题

---@class FeatureShowcase : WorldScript
---@field Speed number 移动速度（单位：米/秒）
---@field RotateSpeed number 旋转角速度（单位：弧度/秒）
---@field ShowHud boolean 是否在屏幕上显示调试 HUD
---@field Label string HUD 上显示的标题文本
---@field Stats FeatureShowcaseStats 数值展示（结构化表：可展开、可编辑、随场景保存）
---@field ExtraInfo table 任意表（只读展示，不进存档）
---@field Scores {number} 分数数组（数组声明：元素按下标成行，可增删）
---@field Config {string: number} 配置映射（映射声明：键名以场景为准，可加键）
---@field Grid {{number}} 二维网格（嵌套数组声明：每个元素本身是数组）
local FeatureShowcase = {
    -- 演示：字段声明与默认值。这些字段由编辑器属性面板反射识别并支持保存到场景
    Speed = 2.0,
    RotateSpeed = 1.0,
    ShowHud = true,
    Label = "WorldEngine Showcase",
    -- 结构化表：子字段按 `FeatureShowcaseStats` 的声明展开；下面给的是默认值
    Stats = {
        Damage = 12.0,
        Range = 3.5,
        Label = "基础数值",
    },
    -- 裸 table：属性面板只显示一行只读摘要，不写进场景
    ExtraInfo = {
        note = "运行期自用，不进场景",
        level = 1,
    },
    -- 数组声明：初值 = 默认形状（3 个元素）。改了/加了元素之后形状按场景保存
    Scores = { 1.5, 2.5, 3.5 },
    -- 映射声明：初值 = 默认键集合（hp/mp）；新增的键会跟着场景保存
    Config = { hp = 10, mp = 20 },
    -- 嵌套数组声明：外层 2 行，每行是一个 2 元素的数组
    Grid = { { 1, 2 }, { 3, 4 } },
    -- 未写 `---@field` 的表：引擎按值推导类型（字符串键 → 可展开的结构化行，初值一起进属性面板）
    InferredStats = {
        Level = 3,
        Title = "自动推导",
        Scale = 1.25,
    },
    -- 未写注解的数组：引擎按值推导成**数组行**（元素类型取自首个元素，初值一起进属性面板）
    RawScores = { 90, 85, 77 },
}

-- 运行时内部状态（非反射导出字段，无需在 @field 中声明）
FeatureShowcase.timerTicks = 0
FeatureShowcase.timerHandle = 0
FeatureShowcase.elapsedTime = 0.1

-- 演示：OnCreate 生命周期。脚本实例启动时被引擎调用一次，适合执行一次性初始化
function FeatureShowcase:OnCreate()
    print(string.format("[FeatureShowcase] OnCreate: 实体 ID = %d, 名称 = '%s'",
        self.entity:GetID(), self.entity:GetName()))
    -- 演示：结构化表的字段在运行期就是普通 Lua 表字段（面板改过的值会被写回这里）
    print(string.format("[FeatureShowcase] Stats: Damage=%.1f Range=%.1f Label='%s'",
        self.Stats.Damage, self.Stats.Range, self.Stats.Label))
    -- 未注解字段同样会在属性面板里出现(推导类型 + 初值):这里读回来验证运行期就是普通 Lua 字段
    print(string.format("[FeatureShowcase] InferredStats: Level=%d Title='%s' Scale=%.2f",
        self.InferredStats.Level, self.InferredStats.Title, self.InferredStats.Scale))
    -- 集合字段在运行期也是普通 Lua 值:数组是连续下标的表,映射是键值表(面板做的增删/改值都会写回这里)
    print(string.format("[FeatureShowcase] Scores: %d 个元素, 第 1 个 = %.1f",
        #self.Scores, self.Scores[1] or 0.0))
    print(string.format("[FeatureShowcase] Config: hp=%.1f mp=%.1f",
        self.Config.hp or 0.0, self.Config.mp or 0.0))
    print(string.format("[FeatureShowcase] Grid: %d 行, 第 1 行第 2 列 = %.1f",
        #self.Grid, (self.Grid[1] and self.Grid[1][2]) or 0.0))

    -- 演示：timers 计时器系统。创建周期性触发的定时器（每 1.0 秒触发一次，0 或缺省表示无限重复）
    self.timerTicks = 0
    self.timerHandle = timers:every(1.0, function()
        self.timerTicks = self.timerTicks + 1
    end)

    -- 演示：timers 单次延迟任务。在 3.0 秒后执行一次回调
    timers:after(3.0, function()
        print(string.format("[FeatureShowcase] 3 秒延迟计时器触发，实体 '%s' 运行正常", self.entity:GetName()))
    end)
end

---@param dt number 距离上一帧经过的秒数（固定或变步长）
-- 演示：OnUpdate(dt) 生命周期。每帧在场景主线程执行，用于驱动实体逻辑与动画
function FeatureShowcase:OnUpdate(dt)
    self.elapsedTime = self.elapsedTime + dt

    -- 演示：动态响应属性变化。每帧直接读取 self.Speed / self.RotateSpeed，编辑器中调整后立即生效
    local currentSpeed = self.Speed
    local currentRotateSpeed = self.RotateSpeed

    -- 演示：通过 self.entity 读取/驱动实体组件（如 TransformComponent）
    if self.entity:IsValid() then
        local transform = self.entity:GetComponent("TransformComponent")
        if transform ~= nil then
            -- 读取当前位置与旋转
            local loc = transform.Location
            local rot = transform.Rotation

            -- 结合 self.Speed 和 dt 计算微小摆动，演示驱动 TransformComponent 位移
            local offsetX = math.sin(self.elapsedTime * currentSpeed) * 0.5 * dt
            transform.Location = vec3.new(loc.x + offsetX, loc.y, loc.z)

            -- 结合 self.RotateSpeed 驱动沿 Z 轴旋转
            local newRotZ = rot.z + currentRotateSpeed * dt
            transform.Rotation = vec3.new(rot.x, rot.y, newRotZ)
        end
    end
end

-- 演示：OnUI 生命周期。每帧调用，用于使用即时模式 UI (ui.*) 绘制游戏内 HUD 面板与调试控件
function FeatureShowcase:OnUI()
    -- 如果属性面板中关闭了 HUD 开关，则跳过绘制
    if not self.ShowHud then
        return
    end

    -- 绘制调试面板背景与标题
    ui.panel(16, 16, 280, 160, self.Label)

    -- 显示静态文本与运行指标
    ui.text(28, 44, string.format("Entity: %s (ID: %d)", self.entity:GetName(), self.entity:GetID()), 14)
    ui.text(28, 66, string.format("Speed: %.2f | Rotate: %.2f", self.Speed, self.RotateSpeed), 14)
    -- 演示：结构化表字段随属性面板/场景保存的值实时生效
    ui.text(28, 88, string.format("Stats: %.1f / %.1f (%s)", self.Stats.Damage, self.Stats.Range,
        self.Stats.Label), 14)
    ui.text(28, 110, string.format("Timer Ticks: %d (%ds)", self.timerTicks, self.timerTicks), 14)

    -- 绘制交互按钮：点击重置计数器
    if ui.button("btn_reset_ticks", 28, 136, 110, 24, "Reset Counter") then
        self.timerTicks = 0
    end

    -- 绘制复选框控件，演示双向控制内部开关
    -- 注意：ui.checkbox 返回更新后的布尔值状态
    self.ShowHud = ui.checkbox("chk_show_hud", 150, 138, 120, 20, "HUD Visible", self.ShowHud)
end

-- 演示：OnDestroy 生命周期。实体被销毁、场景停止播放或热重载前调用，执行资源清理
function FeatureShowcase:OnDestroy()
    print(string.format("[FeatureShowcase] OnDestroy: 实体 '%s' 正在清理资源", self.entity:GetName()))

    -- 演示：手动取消计时器（引擎在实例销毁时也会自动退订所有属于该实例的计时器与事件）
    if self.timerHandle ~= 0 then
        timers:cancel(self.timerHandle)
        self.timerHandle = 0
    end
end

return FeatureShowcase
