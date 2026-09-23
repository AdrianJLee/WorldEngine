# 构建与运行

目标:从零配置 → 构建 → 运行 → 跑测试。命令都可直接复制(powerShell,工作目录 = 仓库根)。

## 前置

- Windows + Visual Studio(含"使用 C++ 的桌面开发"工作负载;本仓库在 `Visual Studio 18 2026` 上验证)。
- CMake ≥ 3.16(本仓库在 `4.3.1-msvc1` 上验证)。
- **Vulkan SDK 必须能配置到**:配置阶段就会查找 Vulkan 头文件与导入库,即使实际用默认的 OpenGL 后端。
  缺失时 `find_package(Vulkan)` 失败,后面的着色器工具链也会退化(见下)。
- 第三方依赖在仓库内(`vendor/`、`third_party/`)与子模块里,**不要**换成系统版本。

## 配置

```powershell
cmake -S . -B build/x64-Debug -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_BUILD_TYPE=Debug -DWORLD_BUILD_SCRIPT_TESTS=ON
```

- 生成器名用本机 `cmake --help` 列表里的;`-A x64` 与 `-G` 一起给。
- `-DWORLD_BUILD_SCRIPT_TESTS=ON` 才 `add_subdirectory(tests)`;**不加就没有 `RUN_TESTS` 目标**。
- 同一构建目录里 `CMAKE_BUILD_TYPE` 与 `--config` 必须同名,否则产物会落在错的路径下。
- 复用已有构建目录前先看 `CMakeCache.txt` 里的源目录 / 生成器 / 平台 / 构建类型是否还匹配。

## 构建

```powershell
# 编辑器(会连带构建 Runtime 与 Game;只改引擎时可把 --target 换成 World)
cmake --build build/x64-Debug --config Debug --target Editor --parallel
```

可用的最小目标:`World`(引擎 `WorldRuntime.dll`)、`Game`(游戏 DLL)、`Runtime`、`Editor`。
产物位置(本仓库当前约定):

| 目标 | 产物 |
| --- | --- |
| `World` | `build/x64-Debug/Editor/Debug/WorldRuntime.dll`(宿主目录各一份) |
| `Game` | `build/x64-Debug/bin/Debug/Game/Debug/Game.dll` |
| `Runtime` | `build/x64-Debug/Runtime/Debug/Runtime.exe` |
| `Editor` | `build/x64-Debug/Editor/Debug/Editor.exe` |

源文件列表在配置阶段用 `GLOB_RECURSE` 收集(没有 `CONFIGURE_DEPENDS`):**增删源文件后要重新配置**。

## 运行

```powershell
# 工作目录必须是仓库根:DLL 与资产都按相对路径查找
.\build\x64-Debug\Editor\Debug\Editor.exe
.\build\x64-Debug\Runtime\Debug\Runtime.exe
```

- 开发期宿主按 `build/x64-<配置>` 的约定找 `Game.dll` / `WorldRuntime.dll`;从仓库根启动最省事。
- 编辑器常用参数:`-scene <路径>` 启动时打开场景;`--ai-control=<port>` 在 `127.0.0.1` 上开自动化控制通道(默认关闭);
  `--cook <目录> [--scene <相对路径>] [--check]` 无窗口打包(见[打包](../user/packaging/README.md));
  `--import-gltf <文件> [--out <目录>] [--logical <内容根下的目录>]` 无窗口导入模型。
- 日志走进程的标准输出/标准错误:从终端启动即可看到。启动闪退时先看这里的最后几行。

## 测试

```powershell
cmake --build build/x64-Debug --config Debug --target ALL_BUILD --parallel
cmake --build build/x64-Debug --config Debug --target RUN_TESTS
```

- **顺序不能反**:`RUN_TESTS` 只驱动 ctest,不会重新构建测试可执行文件。跳过 `ALL_BUILD` 会拿旧测试二进制
  对新的 `WorldRuntime.dll`,结果是假通过或读崩溃。
- 套件按主题注册在 `tests/CMakeLists.txt`(名称形如 `World.Schema`、`World.Wui`、`World.Vfs`、`World.Asset`、
  `World.RhiContract`、`World.VulkanDevice`、`World.ShaderPipeline`、`World.ScriptLifecycle` 等);
  `RUN_TESTS` 的输出里会打印 `N% tests passed, M tests failed out of T`。
- `World.SchemaDrift` 是**字节级门禁**:改了组件 schema 注解却忘了重跑生成器,它会红灯。
- 只想跑单个套件时,直接用与 CMake 同目录的 `ctest`(它通常不在 `PATH` 上):
  `& "<CMake 安装目录>\bin\ctest.exe" --test-dir build/x64-Debug -C Debug -R World.Schema --output-on-failure`。

## 常见失败

| 症状 | 原因 / 处理 |
| --- | --- |
| 配置阶段报 Vulkan 找不到 | 装 Vulkan SDK,或在本次进程里显式给 SDK 根 / CMake 变量;不要猜版本号 |
| 找不到 `RUN_TESTS` 目标 | 配置时没加 `-DWORLD_BUILD_SCRIPT_TESTS=ON`;重新配置同一目录 |
| 改了目录结构但构建没反应 | 源文件列表在配置阶段收集;重新跑 `cmake -S . -B ...` |
| 产物路径和预期不符 | `CMAKE_BUILD_TYPE` 与 `--config` 不同名 |
| DLL 找不到 / `LoadLibraryA` 失败 | 工作目录不对,或宿主与 DLL 的架构(x64)、构建类型不匹配 |
| 着色器编译失败 / 只有 DXIL | `dxc.exe` 需要和 `dxcompiler.dll` 同目录;优先用 Vulkan SDK `Bin/` 里的完整工具链 |

## 内部开发 checkout 的额外工具

内部 checkout 里有一个只读预检脚本(按 `.gitignore` 不进公开仓库):
`tools/agents/check-environment.ps1` —— 一次列出 CMake / MSVC / MSBuild / Windows SDK / Vulkan SDK /
`dxc` / 已有构建缓存的状态,`-AsJson` 供脚本消费。
