# 打包与分发

## 打包命令

```powershell
# 工作目录 = 仓库根;先构建出 Editor
.\build\x64-Debug\Editor\Debug\Editor.exe --cook build\publish

# 只做资产预检(含脚本编译门),不写发行目录
.\build\x64-Debug\Editor\Debug\Editor.exe --cook build\publish --check

# 覆盖起始场景(写进发行清单)
.\build\x64-Debug\Editor\Debug\Editor.exe --cook build\publish --scene assets/scenes/2DTest.wd
```

打包流程依次做:读项目清单 → 增量烘焙资产 → 烘焙着色器 → 把烘焙结果打成内容包 →
拷贝运行时 → 写发行清单。任一步失败会以非零退出码结束并把原因写到标准错误。

## 产物

| 文件 | 是什么 |
| --- | --- |
| `Runtime.exe` | 游戏运行宿主 |
| `WorldRuntime.dll` | 引擎库(发行目录里只有一份) |
| `bin/Game.dll` | 游戏 DLL |
| `packages/Base.wpak` | 内容包,名字来自项目清单的 `packages:` |
| `bin/plugins/<插件名>.dll` | 随包插件(项目清单 `plugins.enabled` + 项目插件默认随包;见下) |
| `project.we.yaml` | 发行清单:起始场景、渲染后端等 |
| `localization/<engine\|project>/<语言>/**` | 语言包副本:引擎层来自 `Engine/assets/localization`,项目层来自项目内容根;编辑器自己的语言包(`Editor/assets/localization`)不进游戏包 |

运行方式:在发行目录里启动 `Runtime.exe`(`Runtime.exe` 是工作目录,资产从内容包解析)。
开发期的中间产物(`<构建目录>/cooked/`)不随发行目录发布。

## 插件随包

包内容由"项目启用了什么"决定(可审计、可复现),不做内容扫描自动塞包:

- **项目插件默认随包**(住在 `<项目根>/plugins/`;插件自己的清单写 `ship: never` 可排除);
- **引擎插件默认不随包**:要随包就在 `project.we.yaml` 的 `plugins.enabled` 里显式列出,
  或者被某个随包插件用 `depends` 拉进来;
- 内容引用了**没随包**的插件(场景里的组件类型、资产类型、导入器产物、脚本里的插件函数)⇒
  **打包失败**,并逐条给出"谁引用了它 → 该启用哪个插件";确需容忍时只能在
  `plugins.tolerate_missing: [插件 id]` 里显式声明(会记 ERROR 日志 + 摘要里计数,不静默);
- 发行目录里的 `project.we.yaml` 会记录实际随包的插件(`plugins.shipped`),`Runtime.exe`
  按这份清单加载 `bin/plugins/*.dll`;插件加载失败会记 ERROR,但不会阻止游戏启动。

## 资源查找

- 开发期宿主按 `build/x64-<配置>` 的相对路径找 DLL 与资产 —— 所以开发时要**从仓库根启动**。
- 发行版从发行目录启动:可执行文件、DLL、内容包都在同一个目录树里,不依赖源码树、也不依赖任何着色器编译器
  (着色器在打包时已烘焙)。

## 常见问题

| 症状 | 原因 / 处理 |
| --- | --- |
| `--cook` 报缺 `Runtime.exe` / `Game.dll` | 先完整构建一次(至少 `Editor`,它会连带构建 `Runtime` 与 `Game`) |
| 运行发行版报缺 DLL | 发行目录被拆开拷贝了;整个目录一起分发 |
| 打包后场景不对 | 用 `--scene` 覆盖,或改项目清单里的 `start_scene` 后重新打包 |
