# WorldEngine

[English](README.md) | 简体中文

WorldEngine 是一款原生 Windows C++17/CMake 游戏引擎，采用模块化动态链接库架构：
引擎核心编译为共享库 **`WorldRuntime.dll`** 并具备显式 `WLD_API` 符号导出边界，
游戏逻辑编译为独立的 **`Game.dll`**，编辑器与运行时作为宿主程序驱动。
第三方依赖与 CLI 工具链随仓库版本锁定（`vendor/`、`third_party/` 与子模块）。

## 模块与产物

| 模块 / 目录 | 产物 | 职责 |
| --- | --- | --- |
| `Engine/` | `WorldRuntime.dll`（CMake 目标 `World`） | 引擎核心：应用与生命周期、作业系统、VFS、渲染 RHI（OpenGL 4.6 / Vulkan）、Slang 着色器管线、ECS 与反射 schema、WUI 框架、关卡 2.0 与数据层、预制体 2.0、Luau 运行时及 2D/3D 物理 |
| `Game/` | `Game.dll` | 游戏玩法逻辑。独立 DLL，通过**显式 ABI 注册接口**接入引擎，不复制引擎单例与全局状态 |
| `Editor/` | `Editor.exe` | 视觉编辑器宿主：schema 驱动的 WUI 面板、多投影模式 3D 视口与自适应标尺网格、关卡与预制体创作、材质/模型检查器、以及视口内运行（Play in Editor） |
| `Runtime/` | `Runtime.exe` | 游戏独立运行宿主：加载项目清单（`project.we.yaml`）与打包资产，不含编辑器 UI 开销 |
| `Launcher/` | `WeEdit.exe` / `WePlay.exe` | 零依赖独立项目启动器（编辑 / 运行模式），支持从任意外部项目根目录双击快速拉起项目 |
| `plugins/` | 插件动态库 | 可扩展引擎插件系统（自定义资产类型、组件、编辑器面板与 Lua 扩展库） |
| `tests/` | CTest 回归套件 | 引擎测试套件（78 项回归测试，涵盖 schema 漂移、运行时契约、组件内存布局、Luau 沙箱/绑定、关卡与预制体 2.0 等） |

依赖方向：宿主（`Editor`、`Runtime`、`Launcher`）与 `Game` 依赖 `World`；`World` 不反向依赖任何宿主。跨模块注册全部通过显式接口完成，不依赖静态初始化顺序。

WorldEngine 同时支持**子项目模式**（`WORLD_AS_SUBPROJECT=ON`）：外部游戏项目可通过 `add_subdirectory(<引擎根>)` 直接将引擎源码内嵌编译。

## 核心架构与特性

- **关卡 2.0 与预制体 2.0 创作管线**：
  - *关卡 2.0（Level 2.0）*：解耦的双文件存储架构，由入口清单（`scenes/<Id>.wlevel`）与单实体文档（`levels/<Id>/entities/*.entity`）成对校验；彻底退役单文件 `.wd` 格式。内置数据层（Data Layers），支持编辑器分层编辑与 Luau 运行时流式加载查询 API。
  - *预制体 2.0（Prefab 2.0）*：自包含单文件 `.wprefab` 模板资产，具备固化 `PrefabLocalId` 映射与行内属性覆写，无需子实体多文件落盘。
  - *OFPA 稀疏放置（Sparse Placement）*：关卡内预制体实例仅落盘模板引用与差异实体，保障磁盘紧凑并实现场景级完整数据往返。
- **现代化 RHI 与 Slang 着色器管线**：
  - 硬件抽象 RHI 支持 **OpenGL 4.6**（默认）与 **Vulkan** 双后端（通过 `project.we.yaml` 配置切换）。
  - 统一采用 **Slang** 编译器 CLI（`slangc`），由 `*.slang` 源码直接编译为双后端目标 SPIR-V，支持参数反射注解与构建期着色器缓存。
- **内嵌 Luau 脚本运行时**：
  - 高性能、沙箱隔离的 Luau 虚拟机，具备基于 schema 反射的薄绑定与玩法系统/组件热重载。
  - 确定性自动生成语言服务器补全声明存根：`<项目根>/assets/scripts/intermediate/WorldEngineAPI.luau`。
- **Schema 驱动的 ECS 与保留模式 WUI**：
  - C++ 组件字段通过注解参与 schema 生成，由生成的访问器自动驱动序列化与属性面板展示。
  - 纯保留模式 WUI UI 框架（100% 控件注册，0 裸绘制违规），编辑器外壳/面板与游戏内 HUD 共用同一套组件库。
  - Viewport 2.0：支持透视（Perspective）与正交（Orthographic）双相机投影、自适应标尺刻度步长、朝向小部件、以及 1500 米前向多 LOD 地面网格。
- **物理与高性能数学**：
  - 深度集成 2D（Box2D）与 3D（Jolt Physics）物理模拟，具备统一的碰撞事件总线。
  - 数学库支持白名单手写 SIMD 指令加速，严格受吞吐门禁约束。
- **项目完全解耦与脚手架**：
  - 项目通过向导创建在引擎目录之外的任意位置，基于模板快速生成（`templates/project-empty`、`templates/project-example`）。

## 快速开始

### 环境依赖

- **操作系统**：Windows 10 / 11（x64）
- **编译器**：Visual Studio 2022 或 2026（包含“使用 C++ 的桌面开发”工作负载）
- **CMake**：≥ 3.16
- **Vulkan SDK**：CMake 配置阶段必需头文件与导入库（即使当前使用默认的 OpenGL 后端）

### 1. 配置项目

```powershell
# Debug / x64 配置，并开启测试目标
cmake -S . -B build/x64-Debug -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_BUILD_TYPE=Debug -DWORLD_BUILD_SCRIPT_TESTS=ON

# （根据本机安装的 Visual Studio 版本，可将生成器替换为 "Visual Studio 18 2026" 等）
```

### 2. 编译构建

```powershell
# 编译全量目标（包含 Editor、Runtime、Game、Launcher 与测试套件）
cmake --build build/x64-Debug --config Debug --target ALL_BUILD --parallel

# 或单独构建编辑器（会自动级联构建 Runtime、Game 与 World）
cmake --build build/x64-Debug --config Debug --target Editor --parallel
```

### 3. 运行回归测试

```powershell
# 运行全部 78 项单元与契约测试
ctest --test-dir build/x64-Debug -C Debug --output-on-failure

# 或通过 CMake 目标运行（需先构建 ALL_BUILD）
cmake --build build/x64-Debug --config Debug --target RUN_TESTS
```

### 4. 运行程序

```powershell
# 运行前请确保工作目录为仓库根目录

# 1) 启动主编辑器
# 不带参数启动将弹出「项目启动器 / 向导」，可新建或选择最近项目：
.\build\x64-Debug\Editor\Debug\Editor.exe

# 或指定打开具体项目：
.\build\x64-Debug\Editor\Debug\Editor.exe --project <项目路径>

# 2) 运行独立游戏客户端
.\build\x64-Debug\Runtime\Debug\Runtime.exe --project <项目路径>

# 3) 项目侧小启动器
# 将 WeEdit.exe / WePlay.exe 复制进任意项目目录，双击即可直接拉起：
.\build\x64-Debug\Launcher\Debug\WeEdit.exe
.\build\x64-Debug\Launcher\Debug\WePlay.exe
```

### 构建与 ABI 避坑守则

- **PDB 并行写入序列化（`/FS`）**：MSVC 多进程并发构建开启 `/FS` 选项，防止并发冲突锁定 PDB（`error C1041`）。
- **显式 `WLD_API` 符号导出**：`WorldRuntime.dll` 严格通过 `WLD_API` 宏显式导出公共接口并启用预编译头（PCH），彻底消除全符号导出（`WINDOWS_EXPORT_ALL_SYMBOLS`）带来的链接负担与 65535 符号溢出风险。
- **运行时内存布局校验守卫**：启动时通过 `WE_RUNTIME_LAYOUT_MATCHES()` 校验宿主与 DLL 间的核心结构体尺寸与成员偏移（`sizeof(Scene)` / `sizeof(WorldContext)`，失配时退出码为 3 并报错）。遇到失配时请重新全量构建，严禁只构建单侧导致混合世代崩溃。
- **配置一致性**：`CMAKE_BUILD_TYPE` 与 `--config` 必须保持一致（同为 `Debug` 或同为 `Release`）。增删源文件后需重新运行 CMake 配置。

## 目录导览

| 路径 | 职责说明 |
| --- | --- |
| `Engine/src/World/Core/` | 应用生命周期、内存分配器、作业系统、VFS、日志、项目清单 |
| `Engine/src/World/Platform/` | Windows 平台层：窗口生命周期、原始输入响应、动态库加载 |
| `Engine/src/World/RHI/` | 渲染硬件抽象接口与 OpenGL 4.6 / Vulkan 双后端实现 |
| `Engine/src/World/Scene/` | 基于 Flecs 的 ECS 核心、关卡 2.0 数据层管理、场景序列化 |
| `Engine/src/World/Schema/` | 反射 schema 注解声明与自动化访问器生成 |
| `Engine/src/World/WUI/` | 保留模式 WUI 控件框架、布局引擎、无障碍树（编辑器与运行时共用） |
| `Engine/src/World/Script/` | Luau 运行时、沙箱绑定、热重载机制、语言服务存根生成器 |
| `Engine/src/World/Physics/` | 2D（Box2D）与 3D（Jolt）物理引擎集成与 ECS 桥接 |
| `Engine/src/World/Math/` | 线性代数、基础几何图元、白名单 SIMD 硬件指令加速 |
| `Engine/assets/shaders/` | Slang 着色器源码（`*.slang`，入口为 `VSMain` / `PSMain`） |
| `Editor/src/WUI/` | 编辑器外壳与面板（场景层级、属性检查器、视口、关卡、数据层、内容浏览器） |
| `Runtime/` | 独立游戏运行宿主（无编辑器 UI 开销） |
| `Launcher/` | 零依赖独立项目启动器（`WeEdit`、`WePlay`） |
| `Game/src/` | 独立游戏 DLL 源码（`Game.dll`），通过显式 ABI 接入 |
| `plugins/` | 引擎可扩展插件目录 |
| `templates/` | 脚手架模板：`project-empty`、`project-example`、`plugin-*` |
| `projects/` | 本地测试项目目录（受 `.gitignore` 保护） |
| `tests/` | 78 项单元与集成回归测试 |
| `tools/agents/` | 环境预检、架构布局门禁、SIMD 吞吐门禁、WUI 检查器等工具套件 |
| `docs/` | 用户手册与开发者架构文档 |

## 资产与文件格式登记

| 扩展名 / 文件 | 类型名称 | 职责说明 |
| --- | --- | --- |
| `project.we.yaml` | 项目清单 | 项目基础配置：内容根路径、默认启动关卡、渲染后端选择（`opengl` / `vulkan`）、物理设置 |
| `.wlevel` | 关卡入口清单 | 关卡清单（`scenes/<Id>.wlevel`）：包含关卡唯一 Id、显示名称、世界网格与数据层配置 |
| `.entity` | 单实体文档 | 独立实体文档（`levels/<Id>/entities/*.entity`）：包含 `ChunkID` 与 `Entity` 两大顶层键 |
| `.wprefab` | 预制体 2.0 资产 | 实体子树模板（`FormatVersion: 2`）：具备固化 `PrefabLocalId` 与行内覆写，无多余子文件 |
| `.wmodel` | 3D 模型资产 | 由 `.gltf` / `.glb` 导入生成的自有模型格式，导入设置保存在资产元数据中 |
| `.wmat` | 材质实例资产 | 材质实例（`FormatVersion: 3`）：引用 Slang 着色器并保存参数覆写与贴图槽位映射 |
| `.slang` | 着色器源码 | Slang 材质与系统着色器源文件，编译输出双后端 SPIR-V |
| `.wpak` | 内容打包分发包 | 供游戏发布的 VFS 只读打包资产库 |
| `.luau` / `.lua` | 脚本代码 | 运行于 Luau 沙箱虚拟机的玩法逻辑与组件行为脚本 |
| `WorldEngineAPI.luau` | IDE 语法补全存根 | 自动生成的代码提示声明文件（`<项目根>/assets/scripts/intermediate/`） |

> 注：旧版单文件场景格式 `.wd` 已彻底退役并被关卡 2.0 分散存储架构取代。

## 文档指引

| 面向受众 | 文档路径 | 涵盖内容 |
| --- | --- | --- |
| 游戏开发者 / 用户 | [`docs/user/`](docs/user/) | 快速入门、编辑器操作、项目结构、资产导入、项目打包、常见问题 |
| 引擎与架构开发者 | [`docs/dev/`](docs/dev/) | 架构全景、构建与运行、模块契约、WUI 控件规范、Slang 着色器标准 |
| 脚本开发者 | [`docs/user/scripting/README.md`](docs/user/scripting/README.md) | Luau 运行时、实体与系统生命周期、Lua 代码提示与工具配置 |
| 项目标准规范 | [`docs/dev/project-standard.md`](docs/dev/project-standard.md) | 标准项目目录布局与脚手架规范 |

## 自动化质量门禁

在提交代码前，可通过 `tools/agents/` 运行自动化门禁：

```powershell
# 编译环境与工具链预检
& .\tools\agents\check-environment.ps1

# 唯一 include 根与目录规范检查
& .\tools\agents\check-layout.ps1

# WUI 控件注册与零裸绘制门禁
& .\tools\agents\check-ui-components.ps1

# SIMD 手写指令与吞吐门禁
& .\tools\agents\check-simd.ps1

# 外部项目规范检查
& .\tools\agents\check-project-layout.ps1 -ProjectRoot <项目路径>
```

## 仓库信息

- **源码仓库**：[https://github.com/AdrianJLee/WorldEngine](https://github.com/AdrianJLee/WorldEngine)
- **默认分支**：`main`（日常研发合并至 `develop` 分支）
- **开源协议**：Apache-2.0，详见 [`LICENSE`](LICENSE) 与 [`NOTICE`](NOTICE)
