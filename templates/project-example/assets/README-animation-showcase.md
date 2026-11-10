# Animation Showcase（AnimGraph 内容示例）

一个打开就能玩的 Milestone 4 示例：通用人物模型站在坡面地形上，WASD 移动、Shift 奔跑、
Space 跳跃、F 瞄准上半身、R 切换 Root Motion，第三人称轨道镜头跟随角色，
AnimGraph 状态实时切换；`G` 可单独开关右手的两骨 IK，直观看到手部跟随移动目标。

## 打开

- 编辑器 Levels 面板双击 `scenes/AnimationShowcase.wlevel`；
- 或直接启动（在仓库根目录执行；`-scene` 按当前工作目录解析）：
  `Editor.exe --project templates/project-example -scene templates/project-example/assets/scenes/AnimationShowcase.wlevel`；
- 或在项目目录下用 Runtime 直接跑（**推荐**，HUD 与脚本 UI 走完整游戏路径）：
  `Runtime.exe`，环境变量 `WLD_START_SCENE=scenes/AnimationShowcase.wlevel`、工作目录 `templates/project-example`。
- 场景已放在 `assets/scenes/` 下，Levels 面板会自动发现；本示例**不修改**项目启动场景。

## 操作

| 输入 | 行为 |
| --- | --- |
| `W` / `S` | 沿镜头前方前进 / 后退（Forward / WalkBack） |
| `A` / `D` | 相对镜头左右侧移（D 朝屏幕右侧；Strafe，2D 混合空间 ±90°） |
| 手柄左摇杆 LX | 左右侧移（轴映射见 `assets/input.weinput`） |
| `Shift` | 奔跑（1.6 → 4.5 m/s） |
| `Space` / 手柄 A | 跳跃（初速 5.2 m/s，重力 -18 m/s²） |
| `F` / 手柄 X | 切换上半身瞄准层（Aim / EnableUpperBodyLayer + AimWeight） |
| `G` | 切换右手 `TwoBoneIkComponent.Enabled`（右手跟随 / 恢复动画姿态） |
| `R` | 切换 Root Motion：脚本位移 ↔ 动画根位移 |
| 鼠标移动 | 旋转第三人称轨道镜头（yaw / pitch，0.12°/px）：鼠标上推 = 抬头、下推 = 低头；Play 默认捕获指针 |
| `←` / `→` | 镜头左转 / 右转（便于无鼠标验收） |
| `↑` / `↓` | 抬头仰视 / 低头俯视（pitch 钳制 -60°..70°） |
| 鼠标滚轮 | 拉近 / 拉远（2.6..7.0 m，平滑插值） |
| `C` | 循环角色朝向模式：跟随镜头 → 朝向移动 → 固定朝向(+Z) |
| `Tab` | 切换镜头跟随 / 自由机位（冻结当前机位） |
| `Esc` / `M` | 释放 / 重新捕获指针（释放后显示光标，可点击 HUD） |

进入 Play 后编辑器立即捕获并隐藏指针（引擎级 PIE 策略，任何项目都生效），
**鼠标移动直接转镜头，不需要按住右键**；按 `Esc` 释放指针即可点击 HUD / 编辑器 Stop，
再按一次回到游戏操作。窗口失焦时引擎不会接收鼠标输入，
不会出现“切到别的程序后镜头还在转”。退出 Play / 切场景时光标与指针锁定会自动恢复。
左上角中文 HUD 实时显示状态、速度、瞄准、根运动、镜头模式/角度/距离、角色朝向模式、
`IK 脚部[开/关] 手部[开/关]` 与最近一次操作。角色朝向为“跟随镜头”时，角色背对镜头，按 `W` 即镜头前方；
为“朝向移动”时，角色平滑转向世界移动方向；为“固定朝向”时始终面向世界 +Z。

**鼠标控制条**（与键盘完全等价，可直接点击验证）：`行走` / `奔跑` / `停止` / `跳跃` /
`瞄准` / `根运动` / `镜头[跟随/自由]` / `坡面演示（脚部 IK）` / `重置位置` /
`镜头左转` / `镜头右转` / `朝向模式` / `镜头复位` / `手部 IK`。
`坡面演示` 会把角色瞬移到缓坡上，便于一眼检查双脚贴合与骨盆下沉；
`手部 IK` 与 `G` 等价，按钮上直接显示当前开关状态。

## 界面语言

本示例的 HUD、鼠标控制条与状态事件均为简体中文；编辑器里的 Animation Graph 面板
（动画图）也会跟随编辑器语言设置显示中文。若编辑器仍是英文，可在
`Project Settings → Editor Preferences → General → Language` 选择“简体中文”，立即生效。

## 场景内容

| 实体 | UUID | 说明 |
| --- | --- | --- |
| `Mannequin Character` | `910000000000000001` | 角色：Transform + SkinnedMeshRenderer + AnimGraph + FootIk + CharacterController |
| `Showcase Camera` | `910000000000000002` | 主相机，FOV 45，near 0.1，far 200 |
| `Showcase Sun` | `910000000000000003` | 方向光 |
| `Showcase Terrain` | `910000000000000004` | 地形，48 m 见方，4 层材质，碰撞开启 |
| `Pillar Left/Right` | `...0005/...0006` | 蓝/橙参照柱 |
| `Showcase Floor` | `910000000000000007` | 安全地面（地形缺失时仍可见、可行走） |
| `Ramp Marker Low/Mid/High` | `...0008/...0009/...0010` | 坡面上的高度参照桩（贴合真实地形高度） |
| `Step Reference 1/2/3` | `...0011/...0012/...0013` | 起点旁的 3 级台阶参照（纯视觉，无碰撞） |
| `Right Hand IK Target` | `910000000000000014` | 右手 IK 目标：青色小球，沿角色右前方做小 8 字运动 |
| `Right Hand IK` | `910000000000000015` | 通用两骨 IK 代理：`UpperArm.R → LowerArm.R → Hand.R`，驱动角色右手 |

资产依赖：

- 角色：`engine/models/characters/mannequin.wmodel`（22 骨、9 条 clip，Y-up、faces +Z、脚底 y=0）；
- 动画图：`engine/animation/mannequin_locomotion.wanimgraph`；
- 地形：`terrains/animation_showcase.wterrain`（65×65、cell 1 m、heightScale 1；实体位于 `(-24,0,-24)`）。
  生成公式见 `Engine/generators/mannequin/generate_showcase_terrain.py`：本地 X 30..44
  smoothstep 升高 2.35 m（世界 X 6..20），之后是 plateau roll，再叠加 basin 与 ±0.055 m ripple。

## IK 是通用系统

本示例同时演示两条完全不同的 IK 链，它们都由同一个通用 `TwoBoneIkComponent` 求解：

- **脚部只是示例驱动**：角色实体上的 `FootIkComponent` 每帧采样地形，为左右脚各发一条
  腿链请求。它本身不含求解数学；`AnimGraphComponent.EnableFootIk` 仅作为兼容回退，
  角色有 `FootIkComponent` 时以组件为准。
- **手部证明通用性**：`Right Hand IK` 代理实体描述 `UpperArm.R → LowerArm.R → Hand.R`，
  目标 `Right Hand IK Target` 每帧沿角色右前方的小 8 字轨迹运动（幅度 ≤ 0.25 m，
  高度约 1.25..1.35 m）。按 `G` 或点控制条按钮可只关闭这条链。
- **任意三骨链**：`RootBone / MidBone / EndBone` 可填手臂、腿、尾巴等任意三骨；
  `OwnerUuid` 指向被驱动角色，`TargetUuid` 指向目标实体（为 0 时改用 `TargetPosition`）。

添加一条新链只需 3 步：

1. 在角色模型上确认三骨链的骨骼名（例如左手 `UpperArm.L / LowerArm.L / Hand.L`）。
2. 新建一个带 `World::TwoBoneIkComponent` 的代理实体，填写 `OwnerUuid`、
   `RootBone / MidBone / EndBone`、`Weight` 与 `PoleAngleDeg`，并设 `Enabled: 1`。
3. 新建一个带 `World::TransformComponent` 的目标实体（可加 `MeshRendererComponent` 便于观察），
   把它的 UUID 填入 `TargetUuid`；脚本/动画移动目标，末端骨骼就会跟随。

## 实现说明与已知边界

- **状态参数由引擎推导**：脚本只写 `CharacterControllerComponent.Velocity` / `Grounded`，
  `AnimGraphSystem` 据此推导 `Speed` / `Direction` / `IsFalling`；脚本不写
  `CurrentStateHash`，也不直接切 clip。
- **角色朝向**：通过 `Transform.Rotation = quat.fromEuler(0, yaw, 0)` 写入角色朝向，
  三种模式（跟随镜头 / 朝向移动 / 固定朝向）均可用；角色 yaw 用最短角指数插值（k≈12），
  越过 ±180° 不会绕远路。三种模式下 AnimGraph 仍由引擎按角色局部速度推导 Direction。
- **地面高度**：优先 `Physics.Raycast`（地形碰撞落地后自动使用精确高度）；
  否则用与 `generate_showcase_terrain.py` **逐项一致**的解析公式求值，见脚本顶部
  `TERRAIN_ORIGIN_X/Z` 与 `profileGround()`。若主 Agent 调整地形生成公式或地形实体位置，
  必须同步这两处，否则角色根节点会与可见坡面错位（Foot IK 会在腿部补偿，表现为腿被拉长/压短）。
- **脚步 IK 驱动**：`Mannequin Character` 上的 `FootIkComponent`（`Weight 1.0`、
  `FootOffset 0.0`、`MaxPelvisDrop 0.35`）是脚步示例的唯一权威开关；
  `AnimGraphComponent.EnableFootIk` 已设为 `0`，避免两条路径同时求解。
- **Root Motion**：默认 `EnableRootMotion = 0`，由脚本位移移动角色；按 `R` 打开后
  位置由 clip 根位移驱动。若所选 clip 没有根位移，角色会留在原地，这是契约允许的预期行为，
  日志会明确说明。Root Motion 开启时脚本不再叠加水平位移，避免双倍移动。
- **第三人称镜头**：相机绕角色做轨道运动（yaw 无限累加，pitch 钳制 -60°..70°），
  位置指数平滑 k≈6，旋转直接跟手；滚轮在 2.6..7.0 m 间平滑缩放。相机位置按地面抬升，
  避免穿地。`Tab` 冻结为自由机位。
  本工程相机局部前向为 -Z，因此观察 yaw 写入 Transform 时加 180°；`camPitch` 为正表示
  镜头升高俯视，欧拉 X 取负值。
- **移动与左右语义**：移动由 `camYaw` 构造正交基——前向 `(sin yaw, 0, cos yaw)`，
  屏幕右向 `(-cos yaw, 0, sin yaw)`（即 `cross(前向, 上)`）。这与引擎 Direction
  “面向 +Z = 0°、角色右侧 = +90°”一致，D 不再反向；HUD 的平移标签先转回角色局部坐标再判断。
- **瞄准与朝向**：Aim 层开启后角色仍保持当前朝向模式；只有“跟随镜头”模式下角色前向
  与镜头前向一致。
- 角色活动范围限制在地形内（世界 X/Z `-22..38`），不会走出可见地面。
- **HUD**：使用 `ui.onDraw` 即时模式绘制（实时状态无法由静态 `.wui` 绑定），
  因此本任务没有新增 `.wui` 文件。
- **编辑器 Play 与 Runtime 的差异**：脚本即显 UI（`ui.onDraw`）目前只在 Runtime 绘制；
  编辑器 Play 里场景、键盘移动与动画都正常，但没有左上角 HUD 与鼠标控制条。
  完整体验请按上面的 Runtime 方式启动。
- **瞄准层**：脚本切换 `AnimGraphComponent.EnableUpperBodyLayer`；`AnimGraphSystem` 把
  “开关=1”直接映射为该层权重参数 `AimWeight=1`（面板显式写参数时仍优先），F 可立即看到
  上半身抬起、下半身继续行走。
- **手柄**：移动 LX、跳跃 A、瞄准 X 通过现有 `assets/input.weinput` 生效；
  LT 奔跑、右摇杆与 R/Tab 在该映射里没有绑定（共享文件不在本任务边界内，未改动）。

## 建议实机步骤

1. 打开关卡并 Play：确认角色可见、Idle 有呼吸动画，相机在角色后上方。
2. W/A/S/D 走一圈：HUD 的状态应在“行走 / 奔跑 / 左平移 / 右平移 / 后退”间切换。
3. 按住 Shift：速度到 4.5 m/s，状态变为“奔跑”；松开回到“行走”。
4. Space：跳跃 → 下落 → 落地，落地后继续移动不抖。
5. 沿 +X 走上坡（世界 X 6..20）：角色骨盆随坡面抬升，脚底贴合；坡顶为平台微起伏。
6. F：上半身进入瞄准层，下半身继续行走；再按 F 恢复。
7. R：日志切换根运动模式；若 clip 无根位移，角色原地不动即为预期。
8. 移动鼠标：镜头绕角色旋转且松开后立即停止；按 `D` 确认角色朝屏幕右侧移动并播放右平移。
9. 滚轮拉近/拉远；方向键旋转 yaw/pitch；按 `Esc` 释放指针后可点击 HUD；按 `C` 检查三种朝向模式与 HUD 行。
10. Tab：镜头冻结；再按 Tab 平滑回到跟随。
11. 鼠标点 `坡面演示（脚部 IK）`：角色站上缓坡，双脚按各自地面高度贴合。
12. 按 `G` 或点 `手部 IK`：青色目标小球持续运动时，右手跟随其位置；再按一次恢复动画姿态，
    HUD 的 `IK 手部[...]` 同步变化。

## 验收截图

见 `tools/agents/tasks/20261023-1400-animgraph-showcase-and-mannequin/screenshots/`：
Runtime 的 Idle/Walk/Run/Jump/Aim/Slope 六张，以及 Anim Graph 编辑器面板整窗截图。


