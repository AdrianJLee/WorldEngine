# WorldEngine 良品游戏研发工程标准 (Good Game Quality Mandate)

本指南是所有人类开发者与 AI Agent 在 WorldEngine 中创建、开发游戏工程时必须遵守的**铁律标准**。违反本标准任何一条交付的产物，一律判定为不合格废品。

---

## 1. 资产真实性铁律 (Anti-Stub Asset Mandate)

- **严禁手写单色方块与拉伸白模**：
  - 严禁用纯单色填充伪造角色贴图（如纯红、纯黑 32x32 方块）；
  - 严禁用拉伸的默认 Cube（Primitive: 0, Mesh: ""）拼凑伪造 3D 载具或建筑。
- **优先复用官方工业级成套资产库**：
  - 2D 资产：直接复用 `templates/project-example/assets/textures/characters/`（`hero_atlas.watlas`, `hero_walk.wanim`, `dungeon_room.wtilemap`, `dungeon_autotile.wruletile` 等）；
  - 音频资产：直接复用 `templates/project-example/assets/audio/`（`footstep_player.wsound`, `impact_metal.waudio`, `ui_click.waudio` 等）；
  - 3D 资产：直接复用 `Engine/content/` 与 `templates/project-example/assets/models/`；
  - VFX 特效：直接复用 `Engine/content/vfx/` 与 `textures/particles/`。

---

## 2. 物理与控制铁律 (Native Physics & Control Mandate)

- **严禁在脚本中手写假物理与假碰撞**：
  - 移动必须使用引擎原生刚体：2D 必须挂载 `World::RigidBody2DComponent` 与 `World::BoxCollider2DComponent`；3D 必须挂载 `World::RigidBody3DComponent`；
  - 墙壁与地面必须有刚体阻挡，严禁在脚本里写硬编码坐标范围（如 `if x > 5 and x < 10 then`）；
- **操作手感三件套必须齐全**：
  - 平台跳跃必须包含：土狼时间 (Coyote Time: 0.1~0.15s)、跳跃预输入缓冲 (Jump Buffer: 0.1s)、起跳即时响应与短按可控切断。

---

## 3. 视口与环境基线铁律 (Viewport & Environment Baseline)

- **杜绝黑屏死视口**：
  - 2D 场景：必须配置正交相机（`CameraComponent::m_ProjectionType = 1`，Zoom 适配视口宽高，Primary = true），必须有环境光或点光源烘托氛围；
  - 3D 场景：必须配置透视相机、主定向光（`DirectionalLightComponent`）与大气天穹/环境漫反射，严禁死黑背景。
- **严禁 ASCII 假 HUD**：
  - 严禁在屏幕上用 `ui.text` 拼凑字符方块（如 `[■■□□□□]`）；
  - UI 必须使用干净的矢量进度条或原生 WUI 控件。

---

## 4. 交付验证与实机验收 (Evidence Verification)

- 交付前必须通过 `Runtime.exe` 启动并截取真实视口画面（`present capture`）；
- 必须通过肉眼视觉检查（`view_image`）：画面必须构图正常、元素丰富、光影自然、操作有反馈、有动作帧播放，严禁纯黑或玩具拼装感。
