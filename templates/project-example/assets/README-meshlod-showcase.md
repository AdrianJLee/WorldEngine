# Mesh LOD 工业级综合展览馆关卡（MeshLODShowcase）

这是一个开箱即用的商业级网格多层次细节 (Mesh LOD) 与空间八叉树 (SceneOctree) 综合内容展览馆。
全面验证了 WorldEngine 交付的**零配置约定式自动 LOD 解析（Zero-Config Auto LOD Resolution）**、多级减面算法、空间八叉树层级视锥剔除与全异步低开销工作流。

---

## 1. 快速打开与体验

- **在编辑器中打开**：
  - 启动 WorldEngine 编辑器后，在左侧 `Levels` 关卡面板中，双击 `scenes/MeshLODShowcase.wlevel` 即可秒级加载进入；
  - 也可通过命令行指定打开：
    ```powershell
    Editor.exe --project templates/project-example -scene templates/project-example/assets/scenes/MeshLODShowcase.wlevel
    ```
- **在 Runtime 中独立运行**（独立游戏模式）：
  ```powershell
  $env:WLD_START_SCENE="scenes/MeshLODShowcase.wlevel"
  .\build\x64-Debug\bin\Runtime.exe --project templates/project-example
  ```

---

## 2. 核心架构与展区设计

### 2.1 零配置约定式自动 LOD 解析 (Zero-Config Auto LOD Resolution)
场景中全部展示实体的 `MeshLODComponent` 槽位（`LOD1Mesh`、`LOD2Mesh`）**均故意留空**：
- 渲染管线 `SceneRenderer` 会根据实体绑定的基础网格路径，自动在同目录下寻找并加载 `<stem>_LOD1.wmodel` 与 `<stem>_LOD2.wmodel`；
- **开发者体验**：为任何网格生成 LOD 后，只需在实体上添加空的 `MeshLODComponent`，即刻全自动无缝生效，无需任何手动拖拽与路径绑定！

### 2.2 五大主题展区 (Five Exhibition Pavilions)
沿中央 Z 轴跑道分布着精心设计的黑金科技感展示台，陈列引擎内容库真实资产：

| 展区编号 | 展区主题 | 展示模型（引擎真实物理资产） | 验证重点 |
| --- | --- | --- | --- |
| **展区 A (Z = 18)** | **自然与植被 (Foliage)** | `scatter_pebble.wmodel`（卵石）<br>`bush_cluster.wmodel`（灌木丛簇群） | 复杂不规则有机形体的轮廓保持与减面保真度 |
| **展区 B (Z = 2)** | **有机角色 (Characters)** | `mannequin.wmodel`（人体模特角色） | 高精网格（1.6 万面）在多级视距下的平滑过渡 |
| **展区 C (Z = -16)** | **硬表面机械与碎石** | `D5Fixture.wmodel`（机械治具组件）<br>`debris_shard.wmodel`（岩体破片） | 平面、硬边特征与材质槽的完整保留 |
| **展区 D (Z = -34)** | **标准几何图元** | `sphere.wmodel`（细分球体）<br>`cube.wmodel`（标准立方体） | 基础曲率表面与立方体的均匀减面特性 |
| **展区 E (Z = -55)** | **远距剔除分界线** | `Far Boundary Marker` 红色警戒栏杆<br>`Distant Object (Culled)` | 超出 50m 距离剔除（kCulled）不提交 GPU 验证 |

---

## 3. 编辑器视口调试能力 (Viewport Debug Modes)

在编辑器视口右上角的 **Show** 下拉菜单中，可自由勾选以下专属视图：

- **Octree Bounds**：在视口中实时绘制空间八叉树节点的 3D 层级 AABB 线框，观察外围四座空间塔楼与展厅的分区包围盒；
- **LOD Coloration**：全场景开启工业标配的伪彩色阶梯调试：
  - **绿色**：LOD 0（原模 100%）
  - **黄色**：LOD 1（50% 中模）
  - **橙色**：LOD 2（25% 远模）
  - **红色**：Culled（超出距离剔除）
- **LOD & Octree HUD**：视口右下角浮动性能卡片，实时显示八叉树总节点数、最大深度、视锥剔除实体数与各 LOD 档位分布统计；
- **选中实体同心圆环**：在视口或 Hierarchy 中选中任何带 `MeshLODComponent` 的实体，视口将在实体周围以 3D 同心圆环绘制 LOD1（绿）、LOD2（黄）、Cull（红）三圈距离，并在头顶标注实时距离与当前生效 LOD。

---

## 4. 运行时交互控制 (Play 模式)

点击视口顶部的 **Play** 或 **Simulate** 运行场景后，脚本系统 `MeshLODShowcaseSystem.luau` 将自动生效：

- **[1] ~ [5] 数字键**：瞬间传送至五大展区的最佳观察机位（[1] 植被展区、[2] 角色展区、[3] 机械展区、[4] 图元展区、[5] 剔除边界）；
- **[Space 空格键]**：开启 / 暂停相机沿 Z 轴自动推拉平滑巡航（相机在 Z = 28m 到 Z = -50m 之间平滑往返，肉眼直观见证全场景柱体的 LOD 阶段流转）；
- **[W / S 键]** 或 **[上 / 下方向键]**：手动前进与后退相机位置，自由观察任意距离的动态过渡效果。
