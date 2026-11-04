# plugins/ (WorldEngine 引擎插件目录)

本目录存放随引擎分发的**一方引擎插件包**。每个子目录代表一个独立的插件单元：
`<引擎根>/plugins/<插件名>/{plugin.we.yaml, CMakeLists.txt, src/**}`。

## 1. 核心构建与加载机制

- **编译构建**：由根 `CMakeLists.txt` 通配收集（开关 `WORLD_BUILD_ENGINE_PLUGINS` 控制），动态链接库（DLL）构建产物统一输出到：
  `build/x64-<配置>/bin/<配置>/plugins/<配置>/`
- **生命周期**：支持宿主环境与独立 MCP 运行时（`EngineBridge::LoadHeadlessPlugins`）自动发现并拓扑排序加载。

---

## 2. 官方核心工作流插件 (we-studio)

依据引擎「Codex + API + 引擎」研发范式，遵循高内聚、反空壳、无 UI 阻塞的工业级原则，核心流程由一方专业插件保障：

| 插件目录 | 插件 ID | 职责定位 | 核心能力 (Provides) | 架构形态 |
| --- | --- | --- | --- | --- |
| **`we-studio`** | `engine.we-studio` | 游戏研发组织流程、40 角色库、本地任务/缺陷系统与立项访谈品味偏好 | `workflow.policy`, `workflow.backlog`, `project.interview` | **纯后台 / Headless / API 优先** |

### 协同工作拓扑 (Codex + API + Engine)
```
外部 AI (Codex / Claude)
         │
         ▼ (MCP JSON-RPC)
Launcher/we-mcp.exe (WeMcp) ──▶ CommandRegistry ──▶ WeAgentCore (状态机 / 门禁守卫)
                                       ▲
                                       │ 动态提供 40 角色、阶段门禁、Backlog 任务与访谈 API
                                  we-studio
```

> **架构注记**：
> 1. 原实验性插件 `we-ai-assistant`（主线程阻塞 HTTP 请求聊天窗口）与 `we-gen`（空壳伪生成）已彻底移除，资产生成与智能对话完全交由外部顶级智能体（Codex）与外部专业管线承接；
> 2. `we-studio` 采用纯后台（Headless）无 UI 架构，无任何 60 FPS 绘制和主线程 I/O 开销，专供 Codex 与 API 通道驱动。
