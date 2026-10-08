# 增强输入系统 (Enhanced Input System) 设计与开发指南

WorldEngine 增强输入系统对标现代商业游戏引擎标准（Unreal Enhanced Input 与 Unity Input System），采用设备层 → 动作层 → 玩法层三段式解耦设计，支持上下文栈、修改器管道、交互触发器、手柄震动、确定性定长录制回放与 AI 无障碍控制。

---

## 1. 架构总览

```
[硬件物理设备] (GLFW 键鼠 / XInput 手柄 / 通用 HID / Win32 IME / 原始鼠标)
      │
      ▼
[RawInputState] (定长 512 位按键位图 + 16 鼠标位图 + 4 槽手柄 16 键/6 轴)
      │
      ▼
[InputRouter] (UI 层优先消费截断 M9 → Mapping Context 栈优先级求值)
      │
      ├──> [InputModifier 管道] (DeadZone, Invert, Scale, Swizzle, ResponseCurve)
      └──> [InputTrigger 状态机] (Pressed, Released, Hold, Tap, DoubleTap, Pulse, Chord)
      │
      ▼
[ActionState / InputSnapshot] (NameId 驻留索引，零堆内存访问)
      │
      ▼
[InputFrame] (定长 64B POD 结构，量化轴，支持 .wreplay 确定性回放与 AI 注入)
```

---

## 2. 核心特性与契约

### 2.1 零堆分配与 NameId 身份契约
- 动作名与轴名在加载/配置期通过 `StringPool` 驻留为 `NameId`（4 字节值类型）；
- 热路径查询全部基于 `NameId` 整数比对与直接数组/位图测试，杜绝 `std::string` 堆分配。

### 2.2 手柄物理后端与热插拔
- **双重后端**：Windows 优先动态加载 XInput（低延迟且支持原生高低频震动马达）；通用手柄自动回退至 GLFW Gamepad 数据库；
- **热插拔事件**：手柄接入与拔出时实时派发 `DeviceConnectedEvent` 与 `DeviceDisconnectedEvent` 至事件总线；
- **震动管理**：通过 `InputService::SetVibration(player, left, right, duration)` 控制震动，支持自然倒计时衰减停止；Luau 暴露 `Input.Rumble`。

### 2.3 映射上下文栈 (Mapping Context Stack)
- 上下文具备独立优先级（Priority）与输入消费策略（Consume）；
- 运行时支持自由压栈/出栈：
  ```cpp
  input.PushContext(vehicleContext, 10); // 上车：载具上下文覆盖常规步行
  input.PopContext(vehicleContext.GetId()); // 下车：恢复常规步行
  ```
- 若高优先级上下文开启 `ConsumeInput`，同键绑定将被阻断，不会穿透至低优先级上下文。

### 2.4 修改器 (Modifiers) 与 触发器 (Triggers)
- **修改器 (`InputModifier`)**：
  - `DeadZone`：支持轴向与径向死区，内置重映射防止死区边缘阶跃；
  - `Invert`：X/Y/Z 轴向反转；
  - `Scale`：标量或向量灵敏度缩放；
  - `ResponseCurve`：非线性幂次曲线，用于精准瞄准与摇杆手感微调；
  - `Swizzle`：维度重排（如将多个数字键映射为 Vec2 XY）。
- **触发器 (`InputTrigger`)**：
  - `Pressed` / `Released`：单帧边沿触发；
  - `Hold`：按住达到阈值时长触发（如蓄力、奔跑）；
  - `Tap`：短按并在限定时间内抬起触发（如互动、轻击）；
  - `DoubleTap`：双击时间窗口触发（如翻滚、闪避）；
  - `Pulse`：持续按住按固定频率连续脉冲触发（如全自动连发）；
  - `Chord`：必须伴随协同按键激活才允许触发（如 Ctrl+S、Shift+Skill）。

### 2.5 确定性定长 `InputFrame` 与回放
- 严格遵循 `sizeof(InputFrame) == 64` 与逐字段 `offsetof` 编译期双重静态断言；
- 抽象统一 `IInputSource`，支持真实设备输入、二进制 `.wreplay` 录制回放与 AI 注入无感知切换。
