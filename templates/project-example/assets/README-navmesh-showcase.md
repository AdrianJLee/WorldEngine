# 3D NavMesh & AI Navigation Showcase（导航寻路与动态避障内容示例）

一个打开就能玩的商业级 3D 导航网格与智能寻路关卡示例：人体模型 AI 寻路代理在具备平缓坡道、陡峭绝壁与中央峡谷的地貌上自适应导航；通过键盘或屏幕左上角 HUD 切换峡谷升降路障，直观见证毫秒级动态重新寻路（Dynamic Repathing）与绕行攀爬高台的震撼效果。

---

## 1. 快速打开与体验

- **在编辑器中打开**：
  - 启动 WorldEngine 编辑器后，在左侧或右侧的 `Levels` 关卡面板中，双击 `scenes/NavMeshShowcase.wlevel` 即可秒级加载进入；
  - 也可通过命令行显式指定打开：
    ```powershell
    Editor.exe --project templates/project-example -scene templates/project-example/assets/scenes/NavMeshShowcase.wlevel
    ```
- **在 Runtime 中独立运行**（推荐，全屏独立游戏体验）：
  ```powershell
  $env:WLD_START_SCENE="scenes/NavMeshShowcase.wlevel"
  .\build\x64-Debug\bin\Runtime.exe --project templates/project-example
  ```
- **预烘焙导航网格说明**：
  关卡自带同名预烘焙导航网格 `assets/scenes/NavMeshShowcase.wnavmesh`。关卡载入时引擎自动探测并无缝反序列化装载，无需手动点击烘焙即可直接进行寻路与游玩。

---

## 2. 交互控制与快捷键指南

进入 Play 模式后，屏幕左上角呈现高保真交互式 WUI HUD 状态卡片。可通过键盘按键或点击 HUD 上的按钮进行实时控制：

| 快捷键 | 屏幕 HUD 按钮 | 功能行为与演示效果 |
| --- | --- | --- |
| `[1]` | `[1] 高台` | 命令 AI 代理前往 **Plateau Hilltop (高台顶峰)**：演示约 25° 平缓坡道爬升与高差贴地导航 |
| `[2]` | `[2] 峡谷` | 命令 AI 代理前往 **Valley Exit (峡谷远端)**：路障开启时走中央直通捷径；路障阻断时自动绕行高台 |
| `[3]` | `[3] 前哨` | 命令 AI 代理前往 **Rocky Outpost (碎石前哨)**：经由东侧阶梯台地缓坡巡航到达 |
| `[4]` | `[4] 营地` | 命令 AI 代理返回 **Base Camp (大本营营地)**：返回平原大本营开阔地 |
| `[Space]` 或 `[O]` | `升降峡谷路障` | **核心交互亮点**：动态切换峡谷路障升降（`CLOSED` 阻断 / `OPEN` 畅通）：<br>• **升起阻断 (`CarveNavMesh = 1`)**：路障升高为红色阻隔网格，NavMesh 实时雕刻打洞，AI 代理检测前方不通，立即在毫秒级内重规划改走西侧高台缓坡绕行；<br>• **降下通行 (`CarveNavMesh = 0`)**：路障沉入地下变为绿色，通行阻断解除，AI 代理立刻选取最近直达峡谷捷径 |
| `[P]` | `循环巡逻` | 开启 / 关闭四大地标多点自动往复巡逻模式（大本营 → 高台 → 峡谷出口 → 碎石前哨 → 大本营） |
| — | `重置 AI 代理位置` | 一键将 AI 代理位置重置回大本营营地初始坐标 `(32, 0, 12)` |

---

## 3. 地貌与空间拓扑设计

关卡地形基于 `assets/terrains/navmesh_showcase.wterrain`（65×65 网格，1.0m 单元格精度，高度场由 `Engine/generators/navmesh/generate_navmesh_showcase.py` 严格数学建模生成）：

1. **Base Camp (大本营营地 - 南侧)**：
   - 坐标 `(32, 0.0, 12)`，平坦无障碍的起步开阔地，地表覆盖草地与细沙，视野开阔；
2. **Central Valley Gateway (中央峡谷走廊)**：
   - 沿 Z 轴自南向北贯穿 `Z = 18 .. 48`，地面平整为泥土步道；
   - 走廊正中 `(32, 1.0, 32)` 矗立着 **Dynamic Barricade (动态升降路障)**，两旁配有工业级安全警戒立柱；
3. **Plateau Hilltop & Gentle Ramps (西侧高台与缓坡)**：
   - 坐标 `(16, 5.5, 32)`，海拔 5.5 米的平坦山巅平台；
   - **南侧爬升缓坡**（`Z = 13 .. 25`）：坡度约 24.6°（< 45° 攀爬极限），通畅无阻；
   - **北侧下行缓坡**（`Z = 39 .. 51`）：坡度约 24.6°，平滑下连至峡谷出口；
   - **东侧断崖绝壁**（`X = 23 .. 28.5`）：坡度约 56°（> 45° 剔除线），在 NavMesh 烘焙阶段被自动剔除，形成无法跨越的险峻悬崖，迫使代理必须沿缓坡规矩行进；
4. **Rocky Outpost (碎石前哨 - 东侧)**：
   - 坐标 `(48, 2.5, 32)`，海拔 2.5 米的岩石观景台地，经由南北两条碎石缓坡连通大本营与峡谷出口。

---

## 4. 实体编排与资产清单

采用标准 OFPA（One File Per Actor）关卡组织形态，所有实体保存在 `assets/levels/NavMeshShowcase/entities/`：

| 实体文件名 | 实体标签 (Tag) | 关键组件与配置参数 | 作用说明 |
| --- | --- | --- | --- |
| `960000000000000001.entity` | `Mannequin Agent` | `SkinnedMeshRendererComponent` (人偶模型)<br>`AnimGraphComponent` (运动动画图)<br>`NavMeshAgentComponent` (速度 4.5m/s，停止距离 0.3m，半径 0.5m，高度 1.8m，自动重规划 1) | AI 智能寻路主角代理 |
| `960000000000000002.entity` | `Valley Barricade` | `NavMeshObstacleComponent` (Extents: 3.0, 2.0, 1.0, CarveNavMesh: 1)<br>`MeshRendererComponent` (Cube 原生图元，颜色动态切换) | 峡谷动态升降路障 |
| `960000000000000003.entity` | `Showcase Terrain` | `TerrainComponent` (Asset: `terrains/navmesh_showcase.wterrain`, CellSize: 1.0) | 演示关卡 3D 真实地形 |
| `960000000000000004.entity` | `NavMesh Showcase Camera` | `CameraComponent` (FOV: 50°, 俯视广角机位，俯仰 -28°) | 第三人称全景观赏相机 |
| `960000000000000005.entity` | `Showcase Sun` | `DirectionalLightComponent` (暖日光，强度 2.2，阴影激活) | 主方向光照明 |
| `960000000000000006 .. 013` | `* Beacon` & `* Pedestal` | `MeshRendererComponent` (高亮彩色发光球体 + 灰色基座，四种颜色标识四大地标) | 空间视觉地标信标 |
| `960000000000000014 .. 015` | `Valley Gate Pillar *` | `MeshRendererComponent` (暗色方柱，界定峡谷物理门禁边界) | 峡谷门禁立柱 |

---

## 5. 3D 导航寻路与避障核心原理

1. **地貌自适应烘焙与坡度剔除 (`NavMesh::BakeFromTerrain`)**：
   - 引擎按配置步长（默认 2.0m）在地形高度场上采样顶点网格，根据双线性中心差分计算表面法向量：
     $$\text{slope} = \arccos(\text{clamp}(N_y, 0, 1)) \times \frac{180}{\pi}$$
   - 若某单元格的坡度大于 `maxSlopeDeg`（45.0°），则判定为险峰峭壁不予生成多边形，实现不可通行物理阻绝；
2. **3D A\* 空间走廊搜索 (`NavMesh::FindPath`)**：
   - 空间任意两点首先经由 `SamplePosition` 正交投影至最近的凸多边形；
   - 随后在凸多边形拓扑邻接图上以欧式中心距离为启发函数（Heuristic）执行 3D A\* 搜索，找出连通走廊；
3. **Funnel 漏斗平滑算法 (String-Pulling)**：
   - 提取走廊相邻多边形相交的门径线段（Portals）；
   - 从起点向终点收紧左右视野顶点，收缩为最短几何平滑折线，彻底消除格子走直角拐弯的生硬锯齿感；
4. **动态障碍物实时雕刻 (`NavMesh::ApplyObstacle`)**：
   - 场景内带有 `NavMeshObstacleComponent` 的动态物体（如本关路的升降门）在启用 `CarveNavMesh` 时，每帧前置对空间包围盒覆盖的所有凸多边形标记 `poly.Blocked = true`；
   - A\* 寻路遍历邻接节点时若遇 `Blocked` 即刻将边权置为无穷大，AI 代理在毫秒级内放弃旧走廊，沿着未受阻的高台缓坡自动重新计算出全新全局最优路径。

---

## 6. 自动化与无头验证

WorldEngine 提供了全量回归测试套件与自动化资产生成器：
- 重新生成地貌与预烘焙网格：
  ```powershell
  python Engine/generators/navmesh/generate_navmesh_showcase.py
  ```
- 运行 NavMesh 核心单元测试（多边形构建、空间采样、A\* 与 Funnel、动态障碍物雕刻、地形烘焙与代理推进）：
  ```powershell
  ctest --test-dir build/x64-Debug -C Debug -R "World\.NavMesh" --output-on-failure
  ```
