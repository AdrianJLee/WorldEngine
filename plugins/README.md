# plugins/ (WorldEngine 引擎插件目录)

本目录存放随引擎分发的**一方引擎插件包**。每个子目录代表一个独立的插件单元：
`<引擎根>/plugins/<插件名>/{plugin.we.yaml, CMakeLists.txt, src/**}`。

## 1. 核心构建与加载机制

- **编译构建**：由根 `CMakeLists.txt` 通配收集（开关 `WORLD_BUILD_ENGINE_PLUGINS` 控制），动态链接库（DLL）构建产物统一输出到：
  `build/x64-<配置>/bin/<配置>/plugins/<配置>/`
- **生命周期**：编辑器启动时（`EditorLayer::InitPlugins`）自动发现并拓扑排序加载。插件可通过编辑器面板 `Window ▸ 插件管理器` 查看状态，或在 `local/plugins.json` 中配置针对当前本机的禁用黑名单。

---

## 2. 官方核心 AI 原生插件库

依据引擎 AI 原生游戏研发架构（参见 `tools/agents/plans/ai-game-dev.md` 与决策 `0001-ai-native-workflow`），本目录提供了三位一体的一方专业插件包：

| 插件目录 | 插件 ID | 职责定位 | 核心能力 (Provides) | 详细介绍 |
| --- | --- | --- | --- | --- |
| **`we-ai-assistant`** | `engine.we-ai-assistant` | 引擎内交互助手与制作人访谈卡片（内门） | `editor.panel`, `llm.provider`, `interview.system` | [README](we-ai-assistant/README.md) |
| **`we-studio`** | `engine.we-studio` | 游戏研发组织流程、40 角色库与本地任务/缺陷系统 | `workflow.policy`, `workflow.backlog`, `editor.ui` | [README](we-studio/README.md) |
| **`we-gen`** | `engine.we-gen` | A+C 混合资产生成管线、DPAPI 凭据托管与风格锁 | `asset.generator`, `credential.manager`, `style.lock` | [README](we-gen/README.md) |

### 协同工作拓扑（两扇门，一条线）
```
外部 AI (Codex/Claude) ─MCP─▶ Launcher/WeMcp.exe (we-mcp.exe) ─┐
                                               ├─▶ CommandRegistry ─▶ WeAgentCore (守卫/门禁/状态机)
编辑器内交互面板 ───────────▶ we-ai-assistant ──┘       ▲
                                                        │ 提供 40 角色/流程规则
                                                  we-studio
                                                        │ 提供 DPAPI/风格锁/生成
                                                   we-gen
```

---

## 3. 自定义插件开发

如需开发新的第三方或项目级插件，推荐使用编辑器 `文件 ▸ 新建插件…` 向导，或直接参考 `templates/plugin-*/` 模板：
- `templates/plugin-empty/`：最小空白插件；
- `templates/plugin-editor-ui/`：注册独立 WUI 编辑器窗口与按钮面板；
- `templates/plugin-asset-type/`：注册引擎新资产格式与 Cook 导入器；
- `templates/plugin-component/`：注册支持反射序列化与属性面板的 ECS 组件；
- `templates/plugin-lua-lib/`：向全局 Luau 沙箱注册 C++ 脚本函数库。

更多插件 ABI 架构规范与内存契约见 `docs/dev/plugin-framework.md`。
