# 3D 投影贴花与凸包物理分解综合展示关卡（DecalConvexShowcase）

这是一个开箱即用的 3D 投影贴花 (Projected Decals) 与凸包物理分解 (Convex Decomposition) 综合内容展示关卡。
全面验证了 WorldEngine 里程碑 7 核心交付特性：**3D 表面贴花投影、掠射角消隐防拉伸 (Normal Angle Cutoff)、真实 3D QuickHull 极值凸包构建与 Jolt 动态刚体复合凸包物理碰撞解算**。

---

## 1. 快速打开与体验

- **在编辑器中打开**：
  - 启动 WorldEngine 编辑器后，在左侧 `Levels` 关卡面板中，双击 `scenes/DecalConvexShowcase.wlevel` 即可秒级加载进入；
  - 命令行快速指定打开：
    ```powershell
    .\build\x64-Debug\bin\Editor.exe --project templates/project-example -scene templates/project-example/assets/scenes/DecalConvexShowcase.wlevel
    ```
- **在 Runtime 中独立运行**（独立游戏模式）：
  ```powershell
  $env:WLD_START_SCENE="scenes/DecalConvexShowcase.wlevel"
  .\build\x64-Debug\bin\Runtime.exe --project templates/project-example
  ```

---

## 2. 核心展区与技术验证设计

关卡分为左右两大专业展区，中央由防护栏隔离，布光与相机焦点开箱即达最佳观赏角度：

### 2.1 3D 贴花投影展区 (Decal Exhibition Zone - 左侧 X < 0)
- **墙面弹孔聚簇 (`Decal - Bullet Hole`)**：
  - 垂直朝向背景墙投射真实金属焦痕与裂纹弹孔贴花，验证局部盒体空间逆模型变换与不透明几何表面的贴合投影。
- **地面血斑 (`Decal - Blood Splatter`)**：
  - 垂直向下投射暗红液体喷溅与微小液滴贴花，验证大面积水平表面贴花混合与 `FadeFactor` 渐变。
- **45° 倾斜斜坡掠射消隐 (`Decal - Dirt Mark (Cutoff Demo)`)**：
  - 投射在倾斜的斜坡与地面边缘，直观演示 `NormalAngleCutoff`（法线夹角超过阈值时平滑消隐），彻底消除了大角度斜面投影拉伸走样。

### 2.2 凸包物理竞技场 (Convex Physics Arena - 右侧 X > 0)
- **凹体机械治具复合凸包 (`Dynamic Concave Body - D5Fixture`)**：
  - 真实工业机械高模，通过 `ConvexDecomposition` 自动切分为 4 个封闭子凸包；
  - 挂载 `RigidBody3DComponent`（Dynamic 动态刚体），开箱即可观察真实多面体在重力下的反弹与滚动。
- **凹面破片岩石复合凸包 (`Dynamic Debris Shard`)**：
  - 不规则锐利凹多面体，验证 3D QuickHull 点云极值包络计算与复杂凹陷面切分。

---

## 3. 运行交互快捷键

点击视口顶部的 **Play** 或 **Simulate** 运行场景后，脚本系统 `DecalConvexShowcaseSystem.luau` 将自动接管交互控制：

| 快捷键 | 交互行为 | 说明 |
| --- | --- | --- |
| **`[1]` 键** | **贴花展区特写机位** | 镜头瞬间推进至左侧贴花墙与斜坡前，近距离检视弹孔细节与掠射消隐 |
| **`[2]` 键** | **凸包物理展区机位** | 镜头切换至右侧物理竞技场俯视广角机位，观察动态多面体跌落与碰撞 |
| **`[Space]` 键** | **凸包刚体高度重置与施力** | 为场景内的动态凸包刚体施加向上跳跃冲量，重复触发高频刚体碰撞与滚动反弹 |

---

## 4. 编辑器视口调试能力

在编辑态下，选中任意实体可直观看到专属 Gizmo 呈现：
- **贴花投影器 Gizmo**：高对比亮橙色立体包围盒，中央附带明显的投影射向法线箭头，正中心绘制十字范围分划线，一眼看懂投影盒体积与投影方向；
- **凸包碰撞体 Gizmo**：绿色高亮多面体棱线与子凸包分面刻线，与物理碰撞外壳 100% 吻合对齐。
