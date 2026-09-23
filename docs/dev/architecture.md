# 架构总览

## 模块与产物

| 模块 | 产物 | 职责 |
| --- | --- | --- |
| `World/` | `WorldRuntime.dll`(`WINDOWS_EXPORT_ALL_SYMBOLS` 导出符号) | 引擎核心:应用与帧循环、内存/作业、VFS 与资产、渲染 RHI、场景与反射、WUI、脚本 |
| `Game/` | `Game.dll` | 游戏逻辑;独立 DLL,经显式接口注册进引擎 |
| `Editor/` | `Editor.exe` | 编辑器宿主 |
| `Runtime/` | `Runtime.exe` | 游戏运行宿主 |
| `tests/` | `World*Tests.exe` + ctest 注册 | 回归测试(需 `-DWORLD_BUILD_SCRIPT_TESTS=ON`) |

## 依赖方向

- 宿主(`Editor` / `Runtime`)与 `Game` 依赖 `World`;`World` **不**反向依赖宿主。
- 跨库注册走**显式接口**(注册表 / 工厂 / 回调):不依赖静态初始化顺序,也不假设跨模块共享单例。
  `Game` 与引擎之间只有公共头文件与导出接口这一条边界。
- CMake 目标之间用 `target_link_libraries` 传递包含路径、编译选项与传递依赖。
- `Runtime` / `Editor` 的依赖链在根 `CMakeLists.txt` 里显式声明,构建 `Editor` 会连带构建 `Runtime` 与 `Game`。

## 引擎内部的分层(速查)

| 层 | 位置 | 说明 |
| --- | --- | --- |
| 应用与基础设施 | `Engine/src/World/Core/` | 应用生命周期、内存、作业、日志、VFS、项目清单 |
| 平台 | `Engine/src/World/Platform/` | 窗口、输入、动态库加载、OpenGL 上下文等平台相关实现 |
| 渲染 | `Engine/src/World/RHI/` | RHI 前端 + OpenGL / Vulkan 后端、资源与描述符抽象 |
| 场景与反射 | `Engine/src/World/Scene/`、`Engine/src/World/Schema/` | ECS、场景序列化、组件定义与生成的访问器 |
| UI | `Engine/src/World/WUI/` | WUI 控件、布局、无障碍节点;编辑器与 Runtime HUD 共用 |
| 脚本 | `Engine/src/World/Script/` | Luau 运行时、绑定、沙箱、热重载、补全声明生成 |

## 渲染

- 引擎默认后端是 **OpenGL**;Vulkan 后端与 OpenGL 后端共用同一套 RHI 前端。
- 后端由项目清单(`Game/project.we.yaml`)的 `renderer: opengl | vulkan` 选择;**字段缺省时按 `opengl` 处理**。
  本仓库示例项目当前写的是 `vulkan`。
- 着色器源是 `Engine/assets/shaders/*.hlsl`(入口 `VSMain` / `PSMain`,`vs_6_0` / `ps_6_0`),
  开发期编译到构建目录的着色器缓存;打包产物里带编译好的着色器,运行时不依赖 `dxc`。

## 场景与反射

- 组件是普通 C++ 结构,字段通过注解参与 schema 生成;生成结果在 `Engine/src/World/Schema/Generated/`。
- 编辑器属性面板是 **schema 驱动**的:字段怎么编辑(名字、分类、取值范围、资产类型、只读核心字段)声明在字段旁,
  不在面板里逐字段硬编码。
- 场景文件(`.wd`)保存实体与组件;`World.SchemaDrift` 测试对生成结果做字节级门禁,改注解必须重跑生成器。

## 资产

- 资产类型有各自的容器与扩展名:`.wmodel`(模型)、`.wmat`(材质)、`.wprefab`(预制体)、`.wd`(场景)、`.wpak`(内容包)。
- `.gltf` / `.glb` 是**导入源**,不是可引用资产:导入产出 `.wmodel`(+ 材质/贴图),引用的是 `.wmodel`。
- 导入设置写在资产自身(`.wmodel` 的元数据块)里,改设置要**重新导入**才生效。

## 脚本

- 引擎内嵌 **Luau**;脚本组件挂到实体上,由引擎驱动 `OnCreate` / `OnUpdate` / `OnDestroy` 等回调。
- 编辑器根据实际注册的绑定生成 `Game/assets/scripts/intermediate/WorldEngineAPI.luau`,供语言服务器做补全;
  生成文件不参与运行,不要 `require`。

## 验证

- 变更按范围验证:编译(最小目标)、`RUN_TESTS` 套件、必要时加端到端脚本。
- 构建与运行口径见[构建与运行](build.md)。
