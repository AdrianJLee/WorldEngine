-- FeatureShowcase.lua
-- WorldEngine 脚本系统特性全景展示示例。
-- 演示脚本声明、生命周期、实体驱动、即时模式 UI、定时器系统以及属性面板交互。

---@class FeatureShowcase : WorldScript
---@field Speed number 移动速度（单位：米/秒）
---@field RotateSpeed number 旋转角速度（单位：弧度/秒）
---@field ShowHud boolean 是否在屏幕上显示调试 HUD
---@field Label string HUD 上显示的标题文本
local FeatureShowcase = {
    -- 演示：字段声明与默认值。这些字段由编辑器属性面板反射识别并支持保存到场景
    Speed = 2.0,
    RotateSpeed = 1.0,
    ShowHud = true,
    Label = "WorldEngine Showcase",
}

-- 运行时内部状态（非反射导出字段，无需在 @field 中声明）
FeatureShowcase.timerTicks = 0
FeatureShowcase.timerHandle = 0
FeatureShowcase.elapsedTime = 0.0

-- 演示：OnCreate 生命周期。脚本实例启动时被引擎调用一次，适合执行一次性初始化
function FeatureShowcase:OnCreate()
    print(string.format("[FeatureShowcase] OnCreate: 实体 ID = %d, 名称 = '%s'",
        self.entity:GetID(), self.entity:GetName()))

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
    ui.text(28, 88, string.format("Timer Ticks: %d (%ds)", self.timerTicks, self.timerTicks), 14)

    -- 绘制交互按钮：点击重置计数器
    if ui.button("btn_reset_ticks", 28, 114, 110, 24, "Reset Counter") then
        self.timerTicks = 0
    end

    -- 绘制复选框控件，演示双向控制内部开关
    -- 注意：ui.checkbox 返回更新后的布尔值状态
    self.ShowHud = ui.checkbox("chk_show_hud", 150, 116, 120, 20, "HUD Visible", self.ShowHud)
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
