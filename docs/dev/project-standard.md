# 项目标准(一个 WorldEngine 游戏项目长什么样)

> 状态:2026-09-28 定稿(PROJ-1)。配套:[`project-layout.md`](project-layout.md)(**引擎仓库**的布局)、
> 用户向 [`../user/projects/README.md`](../user/projects/README.md)、机检 `tools/agents/check-project-layout.ps1`。
> 本页是"一个项目"的规范;引擎仓库自身怎么摆是另一页的事。

## 1. 两层 C++ 的边界(本标准的出发点)

| 层 | 位置 | 归属 | 构建 |
| --- | --- | --- | --- |
| **引擎层 C++** | `Engine/**`(产物 `WorldRuntime.dll`) | 引擎仓库;项目**不复制、不改** | 引擎自己的解决方案 `build/x64-Debug/World.slnx` |
| **游戏项目层 C++** | `<项目根>/src/**`(`Components/`、`Systems/`、`GameProject.cpp`) | 项目自己;组件(数据)与系统(逻辑)都落这里 | 编辑器:内容浏览器「项目 C++」根(与资产同一套网格/列表/搜索,双击走外部 Visual Studio);构建见 §5 |

- **示例内容由示例项目模板提供**(`templates/project-example/**`,包含 4 个场景、示例材质/预制体/脚本/着色器/贴图)。
  **新建项目走向导选择模板**(干净骨架 `templates/project-empty/**` 或示例模板) —— 仓库内不再有内置的 `projects/default`。
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
    Components/               #   schema 组件(纯数据;schema 输入只扫这里)
    Systems/                  #   系统(逻辑,C++ ISystem)
    GameProject.cpp           #   项目系统挂载入口(Attach/Detach,可选文件)
    Generated/                #   生成物(入库,标"不要手改")
  plugins/                    # 可选:项目插件包(<plugins>/<名>/{plugin.we.yaml,src/,CMakeLists.txt})
                              #   由项目构建收集(存在性判断;没有 plugins/ = 零行为变化);
                              #   创建入口:File ▸ New Plugin…(见 plugin-framework.md)
  .vscode/  .luau-lsp/        # 可选:Lua IDE 配置
```

清单字段(与 `ProjectManifest` 同源;由引擎 writer 生成):

- `id`(反域名,如 `com.example.mygame`)、`version`、`content_root`(标准值 `assets`)、
  `start_scene`(相对**内容根**,如 `scenes/Main.wd`)、`renderer`、`rendering:`、`physics:`、`packages:`。

禁止:

- 把构建产物(`.lib/.obj/.pdb/.exe/.dll`)写进项目根(构建目录只放 `build/**` 或项目自己的忽略目录);
- 在 `assets/**` 里放 C++ 源码(`.h/.cpp` 属于 `src/**`);
- 空白项目里出现示例内容(脚本名含 `Example`/`StressTest` 等;示例内容见 `templates/project-example/**`)。

## 3. 创建方式(标准流程)

1. 编辑器 `File ▸ New Project…`:选**模板**(`templates/project-empty/**` 干净骨架或 `templates/project-example/**` 示例内容)+ **位置** + 项目名 → 由仓库模板 +
   引擎 `ProjectManifest::Save` 生成;实现上先写临时目录再整体改名,**永不覆盖**已有内容。
   干净骨架向导默认勾选"包含最小可运行场景(相机 + 方向光)":生成的 `assets/scenes/Main.wd` 里会放一台
   `CameraComponent Primary=true` 的实体与一盏方向光 —— 这是**可运行的最低要求**,不是示例内容
   (示例资产/脚本见 `templates/project-example/**`);不勾则写空场景。
2. 等价手动流程:复制模板目录(`templates/project-empty/**` 或 `templates/project-example/**`)到目标位置改名,再改清单字段。
   两种流程都必须满足 §2。
3. 机检:`pwsh tools/agents/check-project-layout.ps1 -ProjectRoot <项目根>`(exit 0 = 合规;`-ProjectRoot` 必填 ——
   引擎内已无默认项目)。校验"**空白模板**建出的项目不应含示例"时加 `-ExpectNoExamples`;
   示例模板建出的项目本来就带示例,不加这个开关(也不该被误判)。

## 4. 打开与运行

- 编辑器**无参数启动 = 纯项目启动器**(最近项目 / 新建项目 / 新建示例项目 / 打开项目 / 退出),不再默认打开任何项目;
  显式 `--project <目录>` 或环境变量 `WLD_PROJECT_DIR` 直接进编辑器形态。最近项目记录在本机态 `local/projects.json`。

  > `Editor.exe` **无参数 = 纯启动器模式** —— 不挂载任何项目(跳过 Game 模块/存根/面板/窗口恢复),
  > 窗口只有启动器一页;选中项目后以 `--project` 拉起编辑器形态并退出。"启动时打开上次项目"默认改为**关**。
- 切项目走 `File ▸ Open Project…` / `File ▸ Recent Projects ▸ …`(实现上都是带 `--project` 重启编辑器)。
- **启动项目**:`运行 ▸ 启动项目(Runtime)`(与新建成功态的按钮)用独立进程跑当前/目标项目 ——
  它直接调用开发布局里的 `<WLD_OUTPUT_DIR>Runtime/<cfg>/Runtime.exe --project <项目根>`,工作目录 = 项目根。

- 编辑器:`Editor.exe --project <项目根>`(或环境变量 `WLD_PROJECT_DIR=<项目根>`)。
  引擎侧的"当前项目根/内容根"唯一入口是 `World::Paths::ProjectDir()` / `AssetRoot()`;
  CMake 的 `WLD_PROJECT_DIR` / `WLD_ASSETPATH` 只剩"空的 `projects/` 容器"作为编译期默认值(含义是"没有内置项目")。
- Runtime:在项目根(或含清单的目录)启动即可 —— 或用 `--project <项目根>` 指定项目目录。
- 向导里的 `Open Project` = 用 `WLD_PROJECT_DIR` 重启编辑器到该项目(进程级切换,不做同进程热切换)。
- **项目 C++ 的"Open in Visual Studio"**(内容浏览器 ▸ 项目 C++ 根,双击/右键):先找**已经在跑的**
  VS 实例 —— 若有实例打开着该文件所属的根(引擎根/项目根,最长前缀优先),就在那个实例里打开并激活窗口
  (不再起新实例);没有匹配实例才回落"启动新实例"(项目根有 `CMakeLists.txt` 走 CMake 模式,
  引擎文件走 `World.slnx`,其它直接开文件)。枚举与投递都限时 3s,失败静默回落。

### 4.1 项目自带的启动入口(本机产物,名字跟着项目走)

- 向导在项目根复制两个零依赖小启动器,文件名带项目名:`<项目名>-Edit.exe`(起 `Editor.exe --project <项目根>`)、
  `<项目名>-Play.exe`(起 `Runtime.exe --project <项目根>`);构建目录里还没有它们时退回生成
  `<项目名>-Edit.cmd` / `<项目名>-Play.cmd`(`WE_ROOT` 可覆盖引擎根;路径用 `%~dp0.` 规避尾反斜杠陷阱)。
  启动器模式由**编译进 exe 的宏**决定、项目根取 exe 自身所在目录 ⇒ 改名/改名复制都不影响行为。
- 入口是**本机产物**:向导会把这些名字写进项目根的 `.gitignore`(通配 `*-Edit.exe` / `*-Play.exe` /
  `*-Edit.cmd` / `*-Play.cmd`,连同 `/build/`、`/local/`),
  项目本身不需要它们也能被 `--project` 打开 —— 所以机检把它们算**可选件**(缺 = WARN)。
- 小启动器本体在引擎仓库的 `Launcher/`(目标 `WeEdit` / `WePlay`);它不链接引擎,只调宿主 exe。

### 4.2 项目删除契约(PROJ-5)

- 启动器提供 `删除…`(危险色):**永久删除项目目录**,实现用 `std::filesystem::remove_all`,
  **不走回收站、不走 Shell 删除**;必须有**确认框**(危险警告 + 确认 / 取消;PROJ-5 v2 起不再要求输入项目名)。
- 守卫(任一不满足即拒绝,并给可读理由):目录存在且含 `project.we.yaml`;
  不是盘根/UNC 根;不是 `WLD_REPO_ROOT` 本身或其祖先;不在引擎仓库内(避免误删仓库内模板或核心目录);
  不是编辑器当前打开的项目;路径段数 ≥ 2。删除失败时**不**从最近列表移除(避免"看着删了其实还在")。
- 目录**已不存在**的条目:确认框改提示"已不存在"、按钮为 `从列表移除`(只动 `local/projects.json`);
  已存在的目录不再提供"只删列表项"的独立入口(PROJ-5 v2 去掉了 `移除` 按钮)。

## 5. 待办(下一批,需重新确认)

- **项目层 C++ 的独立构建**:`<项目>/src/**` → 项目自带 `CMakeLists.txt` → 生成该项目自己的 Game DLL;
  引擎以导出/SDK 形式被引用(当前还没有)。
- **示例 C++ 进项目层**:`Game/src/{Components,Scripts}`(引擎里的示例)迁到示例模板的 `src/**`,
  项目自带 `CMakeLists.txt`(引擎以"子项目模式"只构建 World+Game),使项目 C++ 在自己的项目里构建与展示。
- 项目级 `.gitignore`/版本控制初始化的向导选项。
