# 项目标准(一个 WorldEngine 游戏项目长什么样)

> 状态:2026-09-28 定稿(PROJ-1)。配套:[`project-layout.md`](project-layout.md)(**引擎仓库**的布局)、
> 用户向 [`../user/projects/README.md`](../user/projects/README.md)、机检 `tools/agents/check-project-layout.ps1`。
> 本页是"一个项目"的规范;引擎仓库自身怎么摆是另一页的事。

## 1. 两层 C++ 的边界(本标准的出发点)

| 层 | 位置 | 归属 | 构建 |
| --- | --- | --- | --- |
| **引擎层 C++** | `Engine/**`(产物 `WorldRuntime.dll`) | 引擎仓库;项目**不复制、不改** | 引擎自己的解决方案 `build/x64-Debug/World.slnx` |
| **游戏项目层 C++** | `<项目根>/src/**`(`Components/`、`Scripts/` 等) | 项目自己;脚本与组件都落这里 | 当前阶段:用外部 Visual Studio 打开项目源码;独立构建见 §5 |

- **示例只属于默认项目** `projects/default`(示例脚本、示例资产、示例场景都在那里)。
  **新建项目不携带任何示例** —— 不许用"复制默认项目"当模板。
- 引擎层与项目层不允许互相污染:项目里不放 `Engine/**`;引擎仓库里不放某个具体项目的业务源码
  (默认项目的示例 C++ 是历史现状,迁移见 §5)。

## 2. 标准项目目录

```
<项目根>/                     # 位置任意(可在仓库外)
  project.we.yaml             # 必需:项目清单(唯一事实源)
  levels.welevel              # 可选:关卡清单(id → 场景)
  assets/                     # 必需:内容根(content_root,资产只写相对它的逻辑路径)
    materials/  models/  prefabs/  scenes/  scripts/  shaders/  textures/
    input.weinput             # 可选:输入映射
    scripts/templates/WorldScript.lua   # 可选:项目自带的 Lua 脚本模板
  src/                        # 必需:游戏项目层 C++
    Components/               #   schema 组件
    Scripts/                  #   脚本(含 schema 生成输入的 `*.h`)
    Generated/                #   生成物(入库,标"不要手改")
  .vscode/  .luau-lsp/        # 可选:Lua IDE 配置
```

清单字段(与 `ProjectManifest` 同源;由引擎 writer 生成):

- `id`(反域名,如 `com.example.mygame`)、`version`、`content_root`(标准值 `assets`)、
  `start_scene`(相对**内容根**,如 `scenes/Main.wd`)、`renderer`、`rendering:`、`physics:`、`packages:`。

禁止:

- 把构建产物(`.lib/.obj/.pdb/.exe/.dll`)写进项目根(构建目录只放 `build/**` 或项目自己的忽略目录);
- 在 `assets/**` 里放 C++ 源码(`.h/.cpp` 属于 `src/**`);
- 非默认项目里出现示例内容(脚本名含 `Example`/`StressTest` 等)。

## 3. 创建方式(标准流程)

1. 编辑器 `File ▸ New Project…`:选**位置** + 项目名 → 由仓库模板 `templates/project/**` +
   引擎 `ProjectManifest::Save` 生成;实现上先写临时目录再整体改名,**永不覆盖**已有内容。
   向导默认勾选"包含最小可运行场景(相机 + 方向光)":生成的 `assets/scenes/Main.wd` 里会放一台
   `CameraComponent Primary=true` 的实体与一盏方向光 —— 这是**可运行的最低要求**,不是示例内容
   (示例资产/脚本只在 `projects/default`);不勾则写空场景。
2. 等价手动流程:复制 `templates/project/**` 到目标位置改名,再改清单字段。
   两种流程都必须满足 §2,不得从 `projects/default` 复制(会带入示例)。
3. 机检:`pwsh tools/agents/check-project-layout.ps1 -ProjectRoot <项目根>`(exit 0 = 合规)。

## 4. 打开与运行

- 编辑器启动时**不再默认落进 `projects/default`**:显式 `--project`/`WLD_PROJECT_DIR` 直接进;
  否则"启动时自动打开上次项目"(偏好,默认开)进最近项目;再否则显示**项目启动器**
  (最近项目 / 新建 / 打开 / 显式"打开默认示例项目")。最近项目记录在本机态 `local/projects.json`。

  > PROJ-3 起:`Editor.exe` **无参数 = 纯启动器模式** —— 不挂载任何项目(跳过 Game 模块/存根/面板/窗口恢复),
  > 窗口只有启动器一页;选中项目后以 `--project` 拉起编辑器形态并退出。"启动时打开上次项目"默认改为**关**。
- 切项目走 `File ▸ Open Project…` / `File ▸ Recent Projects ▸ …`(实现上都是带 `--project` 重启编辑器)。
- **启动项目**:`运行 ▸ 启动项目(Runtime)`(与新建成功态的按钮)用独立进程跑当前/目标项目 ——
  它直接调用开发布局里的 `<WLD_OUTPUT_DIR>Runtime/<cfg>/Runtime.exe --project <项目根>`,工作目录 = 项目根。

- 编辑器:`Editor.exe --project <项目根>`(或环境变量 `WLD_PROJECT_DIR=<项目根>`)。
  引擎侧的"当前项目根/内容根"唯一入口是 `World::Paths::ProjectDir()` / `AssetRoot()`;
  `projects/default` 只是**编译期默认值**,不再是唯一可能。
- Runtime:在项目根(或含清单的目录)启动即可 —— 清单解析按 CWD → `projects/default/` → 开发树。
- 向导里的 `Open Project` = 用 `WLD_PROJECT_DIR` 重启编辑器到该项目(进程级切换,不做同进程热切换)。

### 4.1 项目自带的启动入口(本机产物,名字跟着项目走)

- 向导在项目根复制两个零依赖小启动器,文件名带项目名:`<项目名>-Edit.exe`(起 `Editor.exe --project <项目根>`)、
  `<项目名>-Play.exe`(起 `Runtime.exe --project <项目根>`);构建目录里还没有它们时退回生成
  `<项目名>-Edit.cmd` / `<项目名>-Play.cmd`(`WE_ROOT` 可覆盖引擎根;路径用 `%~dp0.` 规避尾反斜杠陷阱)。
  启动器模式由**编译进 exe 的宏**决定、项目根取 exe 自身所在目录 ⇒ 改名/改名复制都不影响行为。
- 入口是**本机产物**:向导会把这些名字写进项目根的 `.gitignore`(通配 `*-Edit.exe` / `*-Play.exe` /
  `*-Edit.cmd` / `*-Play.cmd`,连同 `/build/`、`/local/`),
  项目本身不需要它们也能被 `--project` 打开 —— 所以机检把它们算**可选件**(缺 = WARN)。
- 小启动器本体在引擎仓库的 `Launcher/`(目标 `WeEdit` / `WePlay`);它不链接引擎,只调宿主 exe。

## 5. 待办(下一批,需重新确认)

- **项目层 C++ 的独立构建**:`<项目>/src/**` → 项目自带 `CMakeLists.txt` → 生成该项目自己的 Game DLL;
  引擎以导出/SDK 形式被引用(当前还没有)。
- **默认项目示例 C++ 迁移**:`Game/src/{Components,Scripts}` → `projects/default/src/**`,
  使默认项目与新项目同一形态(涉及 Game target 源目录与 `GameSchema` 输入发现)。
- 项目级 `.gitignore`/版本控制初始化的向导选项。
