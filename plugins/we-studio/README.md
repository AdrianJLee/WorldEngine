# we-studio (WorldEngine Studio)

WorldEngine AI 原生游戏研发职能流程与任务/缺陷管理系统（AI Game Studio Process, 40 Roles & Local Backlog）。

本插件作为方案 [0001-ai-native-workflow](E:/WorldEngine-docs/knowledge/decisions/0001-ai-native-workflow.md) 的规则与流程引擎（D2），负责承载 4 级组织架构、全品类 40 个专业职能库、7 套研发预设包、8 阶段工业级流水线，以及完全离线的本地 Backlog 与缺陷跟踪体系。

---

## 1. 核心架构与组织模型

### 1.1 四级体系（4 Tiers）
- **L0 出品人（Producer / User）**：真实人类用户，拥有最高裁决权、关键里程碑确认权与 Blocker 覆盖权；
- **L1 制作人（Executive Producer）**：AI 总协调，把控立项类型、整体排期、阶段准入与跨部门调度；
- **L2 部门总监（Directors）**：6 大专业部门负责人（创意、美术、体验、技术、QA、发行），负责把控专业品质与阶段交付件；
- **L3 执行职能（Specialists）**：具体干活的专业执行角色（代码编写、资产建模、关卡搭建、数值配表等）。

### 1.2 六大专业部门与独立 QA 架构（6 Departments）
1. **创意部（Creative）**：创意总监、主策划、关卡设计师、战斗策划、叙事设计师、系统策划、数值策划；
2. **美术部（Art）**：美术总监、概念艺术家、3D 角色/场景艺术家、技术美术（TA）、UI 艺术家、VFX 特效师；
3. **体验部（Experience）**：体验总监、音效设计师、音乐作曲家、UX 设计师；
4. **技术部（Tech）**：技术总监、引擎开发、图形开发、玩法程序、物理动画开发、网络开发、流送与大世界开发、工具链开发；
5. **质检部（QA - 严格独立直属制作人，绝不归技术部领导）**：QA 总监、主测试、玩法平衡 QA、网络与安全 QA、性能与兼容性 QA、自动化 QA；
6. **发行与运营部（Publishing）**：商业化设计师、运营策划、宣发专家。

### 1.3 七套全品类预设包（Presets）
位于 `plugins/we-studio/presets/`：
- `default`：全功能 40 角色标准包；
- `lite`：极简 5 角色轻量包（制作人、策划、程序、美术、测试）；
- `2d-standard`：2D 像素/手绘动作平台跳跃品类；
- `3d-nextgen`：次世代 PBR 写实 3D 品类；
- `3d-stylized`：美式卡通 / 二次元风格化 3D 品类；
- `multiplayer`：多人网络对战 / 联机协作品类；
- `strategy`：深度数值 / 网格战棋 / 卡牌策略品类。

### 1.4 纯本地任务与 Bug 跟踪系统（零外部依赖）
根据决策 D14，任务与 Bug 仅以项目内本地文件为唯一事实来源，拒绝外部复杂云端同步：
- 任务：`<ProjectRoot>/.we/studio/backlog/<stage>/TASK-*.yaml`；
- 缺陷：`<ProjectRoot>/.we/studio/bugs/BUG-*.yaml`。

---

## 2. 编辑器图形界面（WUI）使用说明

1. **呼出工作流看板**：
   - 启动 WorldEngine 编辑器：`build/x64-Debug/Editor/Debug/Editor.exe`；
   - 顶部菜单栏点击 **`Window`（窗口）**；
   - 选择 **`Studio Workflow (工作流看板)`**；
   - 编辑器将展示当前项目的研发状态卡片。
2. **看板核心显示与操作**：
   - **当前研发流水线阶段**：显示当前处于 8 阶段（0 Concept、1 Fun Prototype ... 7 Release）的哪一阶段；
   - **激活预设与角色数**：展示当前激活的预设包（如 `default`）与已唤醒的专业角色；
   - **一键运行工作流验证（`Run Studio Validation`）**：
     点击按钮将即时触发 `studio.validate`，检查项目工件是否合规、是否有未闭环 Blocker 缺陷以及是否存在越权改动，并在面板清晰回显检测报告。

---

## 3. 程序化与 MCP 命令接口

`we-studio` 注册了以下 8 条核心命令：

### 3.1 任务管理命令族 (`task.*`)
- **`task.create`**：创建研发任务
  ```json
  {
    "command": "task.create",
    "args": {
      "title": "实现主角两段跳与土狼时间手感",
      "stage": 1,
      "assignee": "gameplay_programmer",
      "department": "Tech",
      "description": "跳跃手感参数调节，需通过 Playtest 自证"
    }
  }
  ```
- **`task.update`**：更新任务状态（todo / in_progress / review / done）
  ```json
  {
    "command": "task.update",
    "args": {
      "id": "TASK-1-001",
      "status": "done"
    }
  }
  ```
- **`task.list`**：列出任务
  ```json
  {
    "command": "task.list",
    "args": { "stage": 1 }
  }
  ```

### 3.2 缺陷管理命令族 (`bug.*`)
- **`bug.create`**：登记缺陷（严重程度：`blocker`, `critical`, `major`, `minor`）
  ```json
  {
    "command": "bug.create",
    "args": {
      "title": "落地物理碰撞偶尔穿透地面",
      "severity": "blocker",
      "reporter": "qa",
      "assignee": "physics_anim_programmer"
    }
  }
  ```
- **`bug.update`**：更新 Bug 状态（open / fixing / verified / closed）
- **`bug.list`**：检索未闭环 Bug 列表

### 3.3 门禁与评审命令族
- **`review.submit`**：提交阶段交付物评审（`blocker` 级意见将严格挡门，需出品人或总监签署）
- **`studio.validate`**：对当前项目进行工业级合规体检
  - 检查当前阶段必需的工件模板是否存在；
  - 检查是否存在未闭环的 `blocker` 级别 Bug；
  - 检查是否存在未经 AI 通道授信的未归属改动（Untracked Writes）；
  - 校验引擎树核心指纹。

---

## 4. 源码结构

- `src/StudioPolicy.h` / `src/StudioPolicy.cpp`：实现 `IWorkflowPolicy`，驱动阶段状态机、角色的唤醒与休眠、白名单授权；
- `src/BacklogManager.h` / `src/BacklogManager.cpp`：纯本地任务与缺陷的 CRUD 及文件持久化；
- `src/StudioValidator.h` / `src/StudioValidator.cpp`：阶段门禁规则、工件完备性与缺陷阻断校验；
- `src/AgentsDocGenerator.h` / `src/AgentsDocGenerator.cpp`：自动生成项目根目录的 `AGENTS.md` 团队守则；
- `src/StudioPlugin.cpp`：插件 ABI 入口、策略注入与 WUI 面板；
- `presets/**`：包含 7 套全品类预设的 roles 与 process 规则配置。
