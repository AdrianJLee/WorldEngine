# plugins/ (WorldEngine 引擎插件目录)

本目录存放随引擎分发的**一方引擎插件包**。每个子目录代表一个独立的插件单元：
`<引擎根>/plugins/<插件名>/{plugin.we.yaml, CMakeLists.txt, src/**}`。

## 1. 核心构建与加载机制

- **编译构建**：由根 `CMakeLists.txt` 通配收集（开关 `WORLD_BUILD_ENGINE_PLUGINS` 控制），动态链接库（DLL）构建产物统一输出到：
  `build/x64-<配置>/bin/<配置>/plugins/<配置>/`
- **生命周期**：支持宿主环境与独立 MCP 运行时（`EngineBridge::LoadHeadlessPlugins`）自动发现并拓扑排序加载。

## 2. 架构边界注记

- 原实验性插件 `we-ai-assistant`、`we-gen` 及旧流程插件 `we-studio` 已彻底移除；
- 游戏研发工作流、八维机制完备性编译、2D/3D/音频/关卡程序化资产管线统一由外部独立无状态工具链 `E:\AIGameWorkFlow` 承接，通过 `Launcher/we-mcp.exe` (WorldEngine MCP) 非侵入式驱动引擎完成场景组装与实机验证。
