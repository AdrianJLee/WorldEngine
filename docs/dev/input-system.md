# 增强输入系统 (Enhanced Input System) 设计与开发指南

WorldEngine 增强输入系统对标现代商业游戏引擎标准（Unreal Enhanced Input 与 Unity Input System），采用统一源抽象、单一语义源解算、上下文优先级栈、修改器管道、交互触发器、手柄震动、确定性定长录制回放与 AI 无障碍控制。

---

## 1. 架构总览

```
[硬件设备 (GLFW / XInput / RawInput / IME)]   [回放流 (.wreplay)]   [AI 控制通道 (input.inject)]
                  │                                  │                         │
                  ▼                                  ▼                         ▼
         [DeviceInputSource]                [ReplayInputSource]     [Action-Level Injection]
                  │                                  │                         │
                  └─────────────────┬────────────────┘                         │
                                    ▼                                          │
                            [RawInputState]                                    │
                                    │                                          │
                                    ▼                                          ▼
                         [InputService 解算管道] ◄──────────────────────────────┘
                       (Context 栈优先级解算 ─► Modifier 链 ─► Trigger 状态机)
                                    │
                                    ├──────────────────────────┐
                                    ▼                          ▼
                          [InputSnapshot / ActionState]   [InputFrame] (64B POD)
                               (只读查询与 Luau API)        (.wreplay 录制回放与网络)
```

---

## 2. 核心特性与生产契约

### 2.1 唯一语义源与两层解算
- **语义解算**：由 `InputService::EvaluateActionOnStack` 统一负责，遵循两层优先级：
  1. **显式上下文栈**：按 `Priority` 从高到低遍历解算，支持 `ConsumeInput` 截断低优先级同键绑定；
  2. **动作自带 Bindings**：作为最低优先级回退（若配置了 `InputRemapManager` 用户改键覆盖，则优先使用覆盖绑定）。
- **基础上下文自动压栈**：在 `input.weinput` 中标记 `default: true` 的上下文在项目装载时自动压入底层，承载常驻玩法按键；状态类上下文（载具、瞄准、菜单）由脚本按需压栈。

### 2.2 统一输入源抽象 (`IInputSource`)
`InputService` 通过 `SetInputSource(shared_ptr<IInputSource>)` 统一管理输入来源，生产宿主（`GameHost`）不包含任何裸按键轮询：
- **`DeviceInputSource`**：硬件采样源，自动遍历所有注册动作与上下文的按键/鼠标绑定，支持 GLFW 键盘鼠标、`GamepadBackend` 手柄轮询，以及 `WindowsRawInput` 原始高精鼠标增量；UI 捕获指针时（`pointerCaptured`）自动压低鼠标按键；
- **`ReplayInputSource`**：回放源，从 `.wreplay` 的 `InputFrame` 帧流中按帧还原鼠标轨迹与动作激活态；
- **动作级注入 (`Action-Level Injection`)**：AI 控制通道与自动化脚本直接调用 `InputService::InjectAction(player, action, value, frames)`，覆盖层置于解算结果最顶端，不会被平台硬件轮询覆写。

### 2.3 手柄物理后端与热插拔
- **双重后端**：Windows 启动时由 `GamepadBackend::Init()` 动态加载 `xinput1_4.dll`（支持低延迟高低频震动马达）；未命中时回退至 GLFW Gamepad 数据库；
- **热插拔事件**：手柄接入与拔出时实时派发 `DeviceConnectedEvent` 与 `DeviceDisconnectedEvent`；
- **震动管理**：通过 `InputService::SetVibration(player, left, right, duration)` 控制震动，支持自然倒计时衰减停止；Luau 暴露 `Input.Rumble(left, right, duration, player)`。

### 2.4 修改器 (Modifiers) 与 触发器 (Triggers)
- **修改器 (`InputModifier`)**：
  - `DeadZone`：支持轴向与径向死区，内置重映射防止死区边缘阶跃；
  - `Invert`：X/Y/Z 轴向反转；
  - `Scale`：标量或向量灵敏度缩放；
  - `ResponseCurve`：非线性幂次曲线，用于精准瞄准与摇杆手感微调；
  - `Swizzle`：维度重排（如将多个数字键映射为 Vec2 XY）；
  - `Normalize`：向量归一化。
- **触发器 (`InputTrigger`)**：
  - `Pressed` / `Released`：单帧边沿触发；
  - `Hold`：按住达到阈值时长触发（如蓄力、奔跑）；
  - `Tap`：短按并在限定时间内抬起触发（如互动、轻击）；
  - `DoubleTap`：双击时间窗口触发（如翻滚、闪避）；
  - `Pulse`：持续按住按固定频率连续脉冲触发（如全自动连发）；
  - `Chord`：必须伴随协同按键激活才允许触发（如 Ctrl+S、Shift+Skill）。

### 2.5 确定性定长 `InputFrame` 与回放
- 严格遵循 `sizeof(InputFrame) == 64` 与逐字段 `offsetof` 编译期双重静态断言；
- 包含 `ActionButtons`（64 位动作位图）、`QuantizedAxes[16]`（量化轴）、高精度鼠标位置与滚轮；
- 二进制 `.wreplay` 文件包含文件头魔数 `WREP`、种子、步长与 FNV-1a 终态校验哈希。

### 2.6 Win32 原生输入
- **原始鼠标 (`WindowsRawInput`)**：窗口初始化时注册 `RIDEV_INPUTSINK` 设备，拦截 `WM_INPUT` 获取不受 Windows 加速度曲线影响的硬件鼠标增量，供第一人称相机使用；
- **中文输入法 (`WindowsIme`)**：拦截 `WM_IME_STARTCOMPOSITION`、`WM_IME_COMPOSITION` 与 `WM_IME_ENDCOMPOSITION`，获取当前拼音串与光标位置。

---

## 3. Luau 脚本 API (`Input`)

所有方法统一以只读服务全局表 `Input.*` 方式调用：

```luau
-- 1. 基础状态与边沿查询
local isDown = Input.Down("Sprint")         -- 动作是否处于激活态
local isPressed = Input.Pressed("Interact") -- 触发器本帧是否触发 (上升沿)
local isReleased = Input.Released("Jump")   -- 动作是否释放 (下降沿)
local moveAxis = Input.Axis("Move")         -- 合成轴 [-1, 1]

-- 2. 上下文栈管理
Input.PushContext("Vehicle")                -- 激活载具上下文 (高优先级覆盖)
Input.PopContext("Vehicle")                 -- 退出载具上下文 (恢复常规)
local active = Input.HasContext("Vehicle")  -- 检查上下文是否激活

-- 3. 辅助服务与外设
local count = Input.PlayerCount()           -- 当前配置玩家槽位数
local wheel = Input.Scroll()                -- 本帧滚轮纵向增量
Input.Rumble(0.4, 0.6, 0.25)                -- 左马达 0.4, 右马达 0.6, 持续 0.25s
```

---

## 4. 编辑器与质量门禁

- **输入映射面板 (`InputMapPanel`)**：
  - 动作、轴、上下文全量增删与按键重绑（追加/替换）；
  - 映射项上下文配置（优先级、输入消费拦截、默认激活）；
  - 触发器与修改器参数实时可视调节（持续时间、双击间隔、脉冲周期）；
  - 自动向 `<ContentRoot>/input.weinput` 同步落盘，支持未知字段原样保真。
- **质量门禁命令**：
  - `tools/agents/check-input-wiring.ps1`：断言全核心子系统生产接线完整（0 孤立死代码）；
  - `tools/agents/skills/worldengine-dev/scripts/verify-input-semantics.py`：端到端真机按键语义验收；
  - `tools/agents/skills/worldengine-dev/scripts/verify-inputmap-editor.py`：从空项目建映射全流程验收。
