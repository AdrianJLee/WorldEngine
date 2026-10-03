# src/ — 项目层 C++ 导览

`<项目根>/src/**` 是**这个游戏自己的 C++**,随项目走。引擎层 C++ 永远在引擎仓库的
`Engine/**`,它的头文件来自引擎构建产物 —— 不要复制进这里。

## 目录

| 位置 | 放什么 |
| --- | --- |
| `Components/*.h` | **纯数据组件**:`WE_SCHEMA_BODY(Game, <类型>, Component)`。构建期 schema 输入**只扫这个目录**,字段声明进类型注册表,编辑器面板据此生成控件。空模板这里还没有组件 |
| `Systems/*.h` | **逻辑**:`World::ISystem` 的 C++ 系统(一页教程见 `Systems/README.md`) |
| `GameProject.cpp` | **项目系统挂载入口**(见下) |
| `Generated/` | 构建生成物(`Game.manifest` + 注册代码),不要手改 |

## Components:只声明数据

组件描述"实体有哪些数据",逻辑一律不写在组件里。字段声明写法(标量 / 枚举 / 资产 / 嵌套
struct / Array / Map,面板控件与存档规则)见示例项目模板的
`templates/project-example/src/Components/ExampleFeatureComponent.h`。

## Systems 与 GameProject.cpp:逻辑与挂载

逻辑写成 `Systems/*.h` 里的 `World::ISystem`;系统实例**只在一次运行时内存在**,由项目挂载
入口在进入/离开运行时各调一次:

```cpp
// src/GameProject.cpp
void AttachProjectSystems(Scene& scene) { scene.RegisterSystem<MySystem>(); }
void DetachProjectSystems(Scene& scene) { scene.UnregisterSystem<MySystem>(); }  // 与 Name() 逐字相同
```

引擎在 `Scene::OnRuntimeStart` 末尾调 Attach、`OnRuntimeStop` 开头调 Detach(编辑器
Play/Simulate、独立 Runtime、Game 宿主同一条路径)。项目没有 `GameProject.cpp` 时引擎会编一份
空实现,构建不会因为少一个文件而失败。

另一条路是 **Luau 系统脚本**:`assets/scripts/systems/*.luau` 在场景启动时自动加载,用
`ecs:Query` + `ecs:AddSystem` 注册。两者差别:C++ 系统能访问引擎内部的类型与系统 API,Luau
只能通过 `ecs` / `Entity` / `ui` 等暴露的接口。组件 / 系统 / 脚本库三层边界(以及为什么 Luau
不能定义组件)见引擎仓库的 `docs/dev/scripting-architecture.md`。

## 构建与加载

项目自带 `CMakeLists.txt` + `build.cmd`,两种模式产物一致:

- **源码模式(默认)**:引擎以子项目模式被引用,只构建 World 与本项目的 Game 模块;
- **快路径**:`WE_ENGINE_BUILD_DIR` 指向一份已编译好的引擎构建目录时,不再编译引擎,只编本项目。

```bat
build.cmd                      :: Debug(默认)
build.cmd <引擎根> Release      :: 指定引擎根 / 配置
```

产物:`<项目根>/build/x64-<配置>/bin/<配置>/Game/<配置>/Game.dll`(同目录带
`WorldRuntime.dll`)。空模板还没有自定义 C++ 类型,所以这份 Game.dll 目前只是模块骨架;
加了 `src/Components/*.h` 的 `WE_SCHEMA_*` 声明(或 `src/Systems/` 的系统)后重新构建即可。
构建完回到编辑器(用 `--project <项目根>` 打开的项目),`文件 ▸ 重载 C++ 模块` 加载新的
Game.dll —— 编辑器**优先**加载项目构建出来的这一份。

## Luau 不能定义组件

Luau 系统脚本只能写逻辑,**不能**声明组件类型。需要新的数据结构时,在 `src/Components/`
加一个 C++ 组件(纯数据),重新构建后在类型面板里就能看到。
