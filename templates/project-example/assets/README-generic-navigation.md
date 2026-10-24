# Generic 3D Navigation & Multi-Level Architectural Showcase（通用化 3D 导航网格与空间感知示例）

一个打开就能玩的工业级通用 3D 导航网格与空间感知关卡示例：**完全零地貌（Zero Terrain）**，100% 由 `WhiteboxComponent` 原型几何体搭建而成；包含双阶直跑台阶楼梯（Staircase 自动跨接攀爬）、二楼架空回廊与观景阳台（2F Mezzanine 3.0m）、阳台直下跳跃点（Off-Mesh Link 纵身跃下）、单向斜坡引道（Ramp 平缓下行）、代价权衡选路（Cost=0.5 硬化道路 vs Cost=3.0 中庭泥潭）、全场景视线通视检测（LoS Raycast）与自由随机漫游（Wander Mode）。

---

## 1. 快速打开与体验

- **在编辑器中打开**：
  - 启动 WorldEngine 编辑器后，在 `Levels` 关卡面板中双击 `scenes/GenericNavigationShowcase.wlevel` 即可瞬间加载；
  - 也可通过命令行指定打开：
    ```powershell
    Editor.exe --project templates/project-example -scene templates/project-example/assets/scenes/GenericNavigationShowcase.wlevel
    ```
- **在 Runtime 中独立运行**（全屏沉浸式运行体验）：
  ```powershell
  $env:WLD_START_SCENE="scenes/GenericNavigationShowcase.wlevel"
  .\build\x64-Debug\bin\Runtime.exe --project templates/project-example
  ```
- **预烘焙导航网格说明**：
  关卡自带同名二进制导航网格 `assets/scenes/GenericNavigationShowcase.wnavmesh`（464 顶点，370 凸多边形）。关卡载入时由 `Scene::TryAutoLoadNavMesh()` 自动探测并无缝反序列化装载，楼梯、天桥与跳跃点拓扑打开即用，无需手动点击烘焙。

---

## 2. 交互控制与快捷键指南

进入 Play 模式后，屏幕左上角呈现高保真交互式 WUI HUD 实时卡片。可通过键盘或屏幕按钮进行全面交互操作：

| 快捷键 | 屏幕 HUD 按钮 | 功能行为与演示效果 |
| --- | --- | --- |
| `[1]` | `[1] 2F阳台` | **双阶台阶自动跨接攀爬**：AI 代理前往 **2nd Floor Balcony (二楼观景阳台)**。经由西侧 8 级直跑楼梯平滑从 Y=0.0m 爬升至 Y=3.0m 高台天桥，穿过架空回廊抵达悬空阳台 |
| `[2]` | `[2] 后花园` | **区域代价权衡选路**：AI 代理前往 **Courtyard Gate (后花园北门)**。现场演示 AI 自动权衡代价：绕行 Cost=0.5 的环形硬化大道，绝不踏入 Cost=3.0 的中庭深水泥潭！ |
| `[3]` | `[3] 跳跃点` | **跨网格特殊跳跃 (Off-Mesh Link)**：AI 走到二楼阳台边缘一跃而下，经由垂直高差 3.0m 的抛物线轨迹直落一楼减震安全垫 |
| `[4]` | `[4] 入口大厅` | 命令 AI 代理返回 **Entrance Lobby (入口大厅大本营)**：回归开阔起步平原 |
| `[W]` | `开启/关闭自由漫游` | **空间感知随机漫游 (Wander Mode)**：AI 自动调用 `GetRandomPointAround` 算法，在建筑 1F 大厅、2F 阳台、天桥与各走廊间自主随机漫游探索 |
| `[R]` | `执行视线通视检测` | **通用空间感知视线通视 (LoS Raycast)**：发射射线测试 AI 与各大地标间的通视状态，精准检测立柱与建筑隔墙的视线阻挡，并在 HUD 与控制台输出相交证据 |
| — | `重置 AI 位置` | 一键将 AI 代理位置重置回大本营初始坐标 `(20, 0, 4)` |

---

## 3. 白盒多层立体空间设计

关卡为纯白盒室内/建筑关卡，摒弃单一地貌绑定，全部由 `WhiteboxComponent` 实体搭建：

1. **Ground Floor Hall (一楼大厅主地面)**：
   - 尺寸 40m × 40m，中心 `(20, -0.2, 20)`，标高 Y = 0.0m，深色石材地砖；
2. **Grand Staircase (双阶直跑楼梯)**：
   - 位于西侧 `X = 4 .. 8, Z = 8 .. 16`，由 8 级规整台阶组成，总高差 3.0m（单阶踏步高 0.375m，深 1.0m）；
   - 在 NavMesh 拓扑中，相邻台阶在踏步面之间建立连通邻接关系，赋能 AI 顺畅登高；
3. **Second Floor Balcony & Skybridge (二楼架空回廊天桥与观景阳台)**：
   - 标高 Y = 3.0m，由横贯东西的架空回廊天桥（`X = 4 .. 36, Z = 16 .. 20`）与南向悬空观景阳台（`X = 16 .. 24, Z = 20 .. 26`）组成；
   - 俯瞰一楼大厅中庭，视野开阔；
4. **East Descent Ramp (东侧单向斜坡引道)**：
   - 位于东侧 `X = 32 .. 36`，自 `Z = 26`（Y = 3.0m）向南平缓下行至 `Z = 14`（Y = 0.0m），坡度仅 14°，形成单向闭环走行动线；
5. **Courtyard Mud Puddle vs Ring Paved Roads (中庭泥潭与硬化道路)**：
   - **中庭泥潭区**：位于大厅正中 `X: [14, 26], Z: [14, 26]`（12m × 12m），高阻力深水污泥（Cost = 3.0）；
   - **环形硬化道路**：环绕大厅四周（西侧 `X = 6`、东侧 `X = 34`），铺装平整石板（Cost = 0.5）；
   - AI 在前往后花园大门时，经 A* 算法评估，硬化道路总代价（$60\text{m} \times 0.5 = 30$）远低于穿越泥潭代价（$32\text{m} \times 3.0 = 96$），自动做出智慧绕行决策！
6. **Balcony Jump Drop (阳台直下跳跃点)**：
   - 位于二楼阳台正前沿 `(20.0, 3.0, 16.0)`，下方对应一楼 `(20.0, 0.0, 14.0)` 的高能见度减震安全垫；
   - 跨网格链接将上下两层多边形直接连通，赋能特技跳跃与逃生动线；
7. **Architectural Columns & Archway Walls (建筑立柱与隔墙)**：
   - 四根承重方柱位于 `(11, 11)`、`(29, 11)`、`(11, 29)`、`(29, 29)`；
   - 北侧设有一道带有中央门洞（X: [19, 21]）的隔断墙，为 LoS Raycast 视线通视提供现实遮挡物。

---

## 4. 实体编排与资产清单

采用标准 OFPA（One File Per Actor）关卡组织形态，所有实体保存在 `assets/levels/GenericNavigationShowcase/entities/`：

| 实体文件名 | 实体标签 (Tag) | 关键组件与配置参数 | 作用说明 |
| --- | --- | --- | --- |
| `970000000000000001.entity` | `Mannequin Agent` | `SkinnedMeshRendererComponent`<br>`AnimGraphComponent`<br>`NavMeshAgentComponent` (速度 4.5m/s，停止距离 0.3m，自动重规划 1) | AI 智能寻路主角代理 |
| `970000000000000002.entity` | `Ground Floor Hall` | `WhiteboxComponent` (Shape: Box, 40×0.4×40m)<br>`MeshRendererComponent` (Slate grey) | 一楼大厅主地面 |
| `970000000000000003.entity` | `Grand Staircase` | `WhiteboxComponent` (Shape: Stairs, 8 steps, 4×3×8m)<br>`MeshRendererComponent` (Marble) | 双阶直跑台阶楼梯 |
| `970000000000000004.entity` | `Second Floor Balcony` | `WhiteboxComponent` (Shape: Box, 20×0.3×10m)<br>`MeshRendererComponent` (Steel blue) | 二楼架空天桥与阳台 |
| `970000000000000005.entity` | `East Descent Ramp` | `WhiteboxComponent` (Shape: Wedge, 4×3×12m)<br>`MeshRendererComponent` (Light grey) | 东侧下行斜坡引道 |
| `970000000000000006.entity` | `Courtyard Mud Puddle` | `WhiteboxComponent` (Shape: Box, 12×0.06×12m)<br>`MeshRendererComponent` (Mud brown) | 中庭深水泥潭减速区 |
| `970000000000000007 .. 008` | `* Paved Road` | `WhiteboxComponent` (Shape: Box, 3.5×0.04×34m)<br>`MeshRendererComponent` (Golden paved) | 东西两侧硬化大道 |
| `970000000000000009.entity` | `Jump Drop Landing Pad` | `WhiteboxComponent` (Shape: Box, 3.5×0.16×3.5m)<br>`MeshRendererComponent` (High-viz orange) | 阳台跳跃着陆减震垫 |
| `970000000000000010 .. 013` | `* Beacon` | `MeshRendererComponent` (Sphere 原生图元，4色发光地标球体) | 四大空间导航地标 |
| `970000000000000014.entity` | `Generic Navigation Camera` | `CameraComponent` (FOV: 55°, 俯仰 -30° 全景机位)<br>`AudioListenerComponent` | 第三人称主相机 |
| `970000000000000015.entity` | `Showcase Sun` | `DirectionalLightComponent` (暖日光，强度 2.2) | 主环境平行光照明 |
| `970000000000000016 .. 019` | `Pillar *` | `WhiteboxComponent` (Shape: Box, 1.2×5×1.2m) | 四大承重立柱 (视线遮挡物) |
| `970000000000000020 .. 021` | `North Partition *` | `WhiteboxComponent` (Shape: Box, 11×4×0.5m) | 北侧隔断墙体 (视线遮挡物) |

---

## 5. 核心技术原理

1. **全场景几何提取与体素化 (`BakeFromScene` / `BakeFromTriangles`)**：
   - 自动遍历场景内所有 `WhiteboxComponent`、`BoxCollider3DComponent` 与 `MeshCollider3DComponent`，将几何体分解为世界空间三角形面片；
   - 坡度过滤机制：法线坡度 $\le 45^\circ$ 的表面标记为可行走网格；
2. **台阶最大攀爬高 (`StepHeight`) 跨接**：
   - 相邻多边形高差 $\le \text{StepHeight}$（0.45m）时，在多边形邻接表中自动建立共享边；
   - 使得台阶楼梯踏步能够无缝生成连续走廊，实现无需人工插桩的自动楼梯上下攀爬；
3. **区域代价修饰体与加权 A\* (`NavMeshAreaModifier`)**：
   - 在 A\* 启发与累积代价评估中，将走廊边权乘以目标多边形的 `AreaCost`；
   - 使得硬化道路（Cost = 0.5）虽然物理距离更长，但总代价极低，AI 主动做出人性化绕路决策；
4. **跨网格通道 (`Off-Mesh Links`)**：
   - 打破传统仅支持物理平滑相连网格的限制，在空间断裂处（如阳台边缘至地面、断桥跨越、梯子）人工或自动插入特殊边；
   - 赋予 AI 跨层跳跃、特技动作与垂直穿梭的战术机动能力；
5. **空间感知查询接口**：
   - `Raycast(start, end)`：在导航多边形拓扑上直接执行 2D/3D 线段求交，毫秒级判定 AI 与视线目标之间是否存在掩体阻挡；
   - `GetRandomPointAround(center, radius)`：在多边形表面均匀采样随机可达点，驱动自主漫游与逃避行为。

---

## 6. 自动化与无头验证

- 重新生成白盒多层关卡、实体与预烘焙网格：
  ```powershell
  python Engine/generators/navmesh/generate_generic_showcase.py
  ```
- 运行 NavMesh 核心单元测试：
  ```powershell
  ctest --test-dir build/x64-Debug -C Debug -R "World\.NavMesh" --output-on-failure
  ```
