# WorldEngine

[English](README.md) | 简体中文

原生 Windows 游戏引擎:引擎核心编译为共享库 **`WorldRuntime.dll`**,游戏逻辑编译为独立的
**`Game.dll`**,编辑器与运行时是宿主程序。C++17 + CMake,第三方依赖随仓库锁定
(`vendor/`、`Engine/vendor/` 与子模块)。

## 模块

| 模块 | 产物 | 职责 |
| --- | --- | --- |
| `World/` | `WorldRuntime.dll` | 引擎核心:应用与内存、作业、VFS 与资产、渲染 RHI(OpenGL / Vulkan)、场景与反射 schema、WUI 框架、脚本运行时(Luau) |
| `Game/` | `Game.dll` | 游戏逻辑。独立 DLL,通过**显式注册接口**接入引擎,不复制引擎单例 |
| `Editor/` | `Editor.exe` | 编辑器宿主:视口、场景层级、属性、内容浏览器、材质 / 模型 / Prefab 面板、Play |
| `Runtime/` | `Runtime.exe` | 游戏运行宿主:加载项目清单与内容,不含编辑器 UI |
| `tests/` | ctest 套件 | 引擎回归测试(默认不构建,见下) |

依赖方向:宿主(`Editor` / `Runtime`)与 `Game` 依赖 `World`;`World` 不反向依赖宿主。
场景、组件、资产、脚本的跨库注册走显式接口,不依赖静态初始化顺序,也不假设跨模块共享单例。

## 快速开始

前置:Windows、Visual Studio(含"使用 C++ 的桌面开发"工作负载)、CMake ≥ 3.16、
Vulkan SDK(配置阶段就需要头文件与导入库,即使实际使用默认的 OpenGL 后端)。

```powershell
# 1) 配置(Debug / x64;要跑测试就加 -DWORLD_BUILD_SCRIPT_TESTS=ON,否则没有 RUN_TESTS 目标)
cmake -S . -B build/x64-Debug -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_BUILD_TYPE=Debug -DWORLD_BUILD_SCRIPT_TESTS=ON

# 2) 构建(Editor 会连带构建 Runtime 与 Game;只改引擎时可把 --target 换成 World)
cmake --build build/x64-Debug --config Debug --target Editor --parallel

# 3) 测试:先 ALL_BUILD,再 RUN_TESTS
#    RUN_TESTS 只驱动 ctest,不会重新构建测试二进制;顺序反了会拿旧二进制对新 DLL。
cmake --build build/x64-Debug --config Debug --target ALL_BUILD
cmake --build build/x64-Debug --config Debug --target RUN_TESTS

# 4) 运行(工作目录 = 仓库根)
.\build\x64-Debug\Editor\Debug\Editor.exe
.\build\x64-Debug\Runtime\Debug\Runtime.exe
```

- 生成器名按本机 `cmake --help` 列出的可用生成器替换;本仓库在 `Visual Studio 18 2026` 上验证过。
- `CMAKE_BUILD_TYPE` 与 `--config` 要同名(都用 `Debug` 或都用 `Release`),否则产物路径会错位。
- 增删源文件后要重新配置:源文件列表在配置阶段收集(没有 `CONFIGURE_DEPENDS`)。

## 目录导览

| 路径 | 是什么 |
| --- | --- |
| `Engine/src/World/Core/` | 应用、内存、作业、VFS、项目清单 |
| `Engine/src/World/Scene/` | ECS、场景序列化、组件定义 |
| `Engine/src/World/Schema/` | 反射 schema 与生成的访问器 |
| `Engine/src/World/RHI/` | 渲染前端与 OpenGL / Vulkan 后端 |
| `Engine/src/World/WUI/` | WUI 控件与布局框架(编辑器与 Runtime HUD 共用) |
| `Engine/src/World/Script/` | Luau 运行时、绑定与热重载 |
| `Engine/assets/shaders/` | HLSL 着色器源(`*.hlsl`,入口 `VSMain` / `PSMain`) |
| `Game/assets/scenes/` | 示例场景(`.wd`) |
| `Game/assets/scripts/` | 游戏脚本与补全声明 |
| `Editor/src/WUI/` | 编辑器面板与外壳 |
| `tests/` | 各主题的测试可执行文件与 ctest 注册 |
| `docs/` | 公开文档(见下) |

## 文档

| 面向 | 位置 |
| --- | --- |
| 做游戏的人 | [`docs/user/`](docs/user/) —— 入门、编辑器、资产、脚本、打包、常见问题 |
| 改引擎 / 构建 | [`docs/dev/`](docs/dev/) —— 构建与运行、架构、扩展点 |
| 脚本与代码提示 | [`docs/scripting/lua-tooling.md`](docs/scripting/lua-tooling.md) |

## 资产与脚本类型

| 扩展名 | 是什么 |
| --- | --- |
| `.wd` | 场景文件(实体与组件) |
| `.wmodel` | 模型资产:由 `.gltf` / `.glb` 导入产生,导入设置保存在资产自身 |
| `.wmat` | 材质资产 |
| `.wprefab` | 预制体 |
| `.wpak` | 内容包(打包产物) |
| `.lua` / `.luau` | 脚本;引擎内嵌的脚本运行时是 Luau |
| `WorldEngineAPI.luau` | 编辑器生成的补全声明(`Game/assets/scripts/intermediate/`),不要手改 |

## 仓库

- 源码:https://github.com/AdrianJLee/WorldEngine
- 默认分支:`main`(开发在 `develop` 分支)
- 许可:Apache-2.0,见 [`LICENSE`](LICENSE)
