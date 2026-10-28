# Mesh LOD 与 SceneOctree 空间八叉树综合展示关卡（MeshLODShowcase）

这是一个开箱即用的工业级网格多层次细节 (Mesh LOD) 与空间八叉树 (SceneOctree) 综合内容示例关卡。
关卡直观展现了 WorldEngine 里程碑 6 交付的连续距离网格切换、防抖滞后阻尼、距离剔除以及空间八叉树加速渲染管线。

---

## 1. 快速打开与体验

- **在编辑器中打开**：
  - 启动 WorldEngine 编辑器后，在左侧的 `Levels` 关卡面板中，双击 `scenes/MeshLODShowcase.wlevel` 即可秒级加载进入；
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

## 2. 核心技术展示点

1. **多阶梯距离减面与距离剔除 (Distance LOD & Culling)**：
   - 沿中央 Z 轴直线排布的 4 组实体展示：
     - **LOD 0 (近景)**：位于 `Z = 16`，相机距离 < 15m，展示高精网格与原始基色；
     - **LOD 1 (中景)**：位于 `Z = 4`，相机距离 15m ~ 35m，自动降级至 LOD1；
     - **LOD 2 (远景)**：位于 `Z = -18`，相机距离 35m ~ 65m，自动降级至 LOD2 简模；
     - **Culled (剔除)**：位于 `Z = -45`，相机距离 > 65m，超出 CullDistance 直接不提交渲染。
2. **滞后防抖阻尼 (Hysteresis Damping 2.0m)**：
   - 当相机在 15m / 35m / 65m 边界处微小晃动时，5% 阻尼带可防止画面发生频繁剧烈的 LOD 跳变和闪烁。
3. **空间八叉树视锥剔除与空间分区 (SceneOctree Partitioning)**：
   - 场景四周四个象限布置了 4 座高耸的空间分区塔楼（(+X,+Z), (-X,+Z), (+X,-Z), (-X,-Z)），直观展现八叉树节点对 3D 空间的层级包围划分。

---

## 3. 编辑器视口调试能力 (Viewport Debug Modes)

在编辑器视口右上角的 **Show** 下拉菜单中，可自由勾选以下专属视图：

| 调试视图 | 功能与视觉效果 |
| --- | --- |
| **Octree Bounds** | 在视口中实时绘制空间八叉树节点的 3D 层级 AABB 线框，直观查看物体所处的八叉树节点划分与深度 |
| **LOD Coloration** | 全场景开启行业标配的 LOD 阶梯变色调试：<br>- **绿色**：LOD 0<br>- **黄色**：LOD 1<br>- **橙色**：LOD 2<br>- **红色**：Culled 剔除状态 |
| **LOD & Octree HUD** | 视口右下角浮动性能卡片，实时显示八叉树总节点数、最大深度、视锥剔除实体数与各 LOD 档位分布统计 |
| **选中实体同心圆环** | 在视口或 Hierarchy 中选中任何带 `MeshLODComponent` 的实体，视口将在实体周围以 3D 同心圆环绘制 LOD1（绿）、LOD2（黄）、Cull（红）三圈距离，并在头顶标注实时距离与当前生效 LOD |

---

## 4. 运行时交互控制 (Play 模式)

点击视口顶部的 **Play** 或 **Simulate** 运行场景后，脚本系统 `MeshLODShowcaseSystem.luau` 将自动生效：

- **[Space 空格键]**：开启 / 暂停相机沿 Z 轴自动推拉巡航（相机在 Z = -30 到 Z = 30 之间平滑往返，肉眼直观见证整个场景各柱体的 LOD 阶段流转）；
- **[W / S 键]** 或 **[上 / 下方向键]**：手动前进与后退相机位置，自由观察任意距离的动态过渡效果。

