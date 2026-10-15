# we-ai-assistant (WorldEngine AI Assistant)

WorldEngine 引擎内 AI 交互助手与制作人访谈卡片系统（In-Engine AI Conversational Assistant & Producer Interview System）。

本插件作为方案 [0001-ai-native-workflow](E:/WorldEngine-docs/knowledge/decisions/0001-ai-native-workflow.md) 的“内门”（Inside Gate），负责提供编辑器内的图形化交互面板与对话上下文聚合，打通“只有想法”的用户（L0 出品人）与引擎层级 AI 架构之间的第一道触点。

---

## 1. 核心特性

- **制作人 3 题访谈卡片（Producer Interview Cards）**：
  严格遵循工业级减负设计，每个研发阶段向出品人展示 ≤3 道关键决策题目（如品类选择、美术基调、预期规模），每题均附带明确的专业推荐选项（`* [Rec]`），避免开放式发散。
- **品味档案与假设自动沉淀**：
  出品人的选择会自动原子化落盘至项目根目录：
  - `taste.yaml`：持久化项目品味偏好（美术风格、题材倾向等），供后续资产生成与策划参考；
  - `assumptions.md`：记录关键立项假设与决策时间戳，作为架构与审计证据。
- **多角色多层级对话**：
  支持直接与 L1 制作人（宏观排期与指导）、QA 质检总监（门禁自证与试玩要求）、技术总监等不同职能角色对话，各层级上下文独立隔离。
- **WUI 可视化面板与命令通道双模**：
  既可以在编辑器 GUI 中通过点选按钮直观交互，也可以由外部 Agent 通过 `we-mcp` 或 AI 控制通道程序化调度。

---

## 2. 编辑器图形界面（WUI）使用说明

1. **呼出面板**：
   - 启动 WorldEngine 编辑器：`build/x64-Debug/Editor/Debug/Editor.exe`；
   - 在顶部菜单栏点击 **`Window`（窗口）**；
   - 在下拉菜单中选择 **`AI Assistant (AI 助手)`**；
   - 编辑器将弹出专属的浮动/停靠面板。
2. **立项决策交互**：
   - 启动初期（Stage 0 Concept），面板直接呈现 3 题访谈卡片：
     - **游戏品类**：例如 `2D 动作平台跳跃 (Recommended)` / `3D 次世代动作` / `深度数值策略`；
     - **美术风格**：例如 `复古 16-bit 精品像素 (Recommended)` / `美式卡通/二次元风格化` / `次世代写实`；
     - **项目规模**：例如 `精品独立小品 (Recommended)` / `Game Jam 极速原型`；
   - 点击选项按钮即可完成答题，面板顶部即时回显记录成功，并自动生成/更新 `taste.yaml` 与 `assumptions.md`。
3. **快捷指令**：
   - 点击 `Chat with L1 Producer: Kickoff Concept` 触发制作人发起当前阶段的立项推进；
   - 点击 `Chat with QA Director: Verify Gates` 呼叫 QA 总监运行当前阶段门禁。

---

## 3. 程序化与 MCP 命令接口

`we-ai-assistant` 向引擎 `World::Ai::CommandRegistry` 注册了以下 4 条标准命令，外部 AI（Codex/Claude）经 `we-mcp.exe` 或编辑器控制通道均可直接调用：

### 3.1 `assistant.interview.get` (获取访谈卡片)
- **Effect**: `Read`
- **参数**:
  - `stage` (int): 研发阶段索引（0=概念, 1=找乐原型, 等）；
  - `root` (string, 可选): 项目根路径。
- **调用示例**:
  ```json
  {
    "command": "assistant.interview.get",
    "args": { "stage": 0 }
  }
  ```
- **返回结构**:
  ```json
  {
    "round": 1,
    "questions": [
      {
        "id": "genre_type",
        "header": "游戏品类",
        "prompt": "您最想做的核心玩法与题材方向是什么？",
        "options": [
          { "id": "2d_platformer", "label": "2D 动作平台跳跃 (Recommended)", "description": "爽快手感、精准跳跃与像素/手绘风格", "recommended": true },
          { "id": "3d_action", "label": "3D 次世代动作", "description": "第三人称写实、PBR 高精度与连招战斗", "recommended": false }
        ]
      }
    ]
  }
  ```

### 3.2 `assistant.interview.answer` (提交回答并落盘)
- **Effect**: `Write`
- **参数**:
  - `question` (string, 必需): 题目 ID；
  - `option` (string, 必需): 选中的选项 ID；
  - `root` (string, 可选): 项目根路径。
- **调用示例**:
  ```json
  {
    "command": "assistant.interview.answer",
    "args": {
      "question": "genre_type",
      "option": "2d_platformer"
    }
  }
  ```
- **执行效果**:
  - 在 `<ProjectRoot>/taste.yaml` 追加 `genre_type: "2d_platformer"`；
  - 在 `<ProjectRoot>/assumptions.md` 追加时间戳决策记录。

### 3.3 `assistant.taste.get` (读取项目品味档案)
- **Effect**: `Read`
- **参数**:
  - `root` (string, 可选): 项目根路径。
- **调用示例**:
  ```json
  {
    "command": "assistant.taste.get",
    "args": {}
  }
  ```

### 3.4 `assistant.chat` (职能总监对话)
- **Effect**: `Read`
- **参数**:
  - `message` (string, 必需): 对话内容；
  - `role` (string, 可选): 角色 ID（`producer`, `qa`, `tech_director` 等，缺省为 `producer`）。
- **调用示例**:
  ```json
  {
    "command": "assistant.chat",
    "args": {
      "role": "producer",
      "message": "我们准备开始制作 2D 动作平台跳跃手感原型"
    }
  }
  ```

---

## 4. 源码结构

- `src/InterviewSystem.h` / `src/InterviewSystem.cpp`：访谈题库、卡片生成、偏好写入与持久化逻辑；
- `src/AssistantEngine.h` / `src/AssistantEngine.cpp`：职能对话应答与命令封装；
- `src/AssistantPlugin.cpp`：插件 ABI 入口、WUI 面板注册与绘制回调（`DrawAssistantPanel`）；
- `plugin.we.yaml`：插件清单（scope: engine, ship: never）。
