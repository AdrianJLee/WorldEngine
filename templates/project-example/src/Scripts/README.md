# src/ — 游戏项目层 C++

这个目录属于**项目**(`<项目根>/src/**`),随项目走;`Components/` 放项目自己的组件,
`Scripts/` 放项目自己的脚本。目录保持可扩展,新建的空项目里只有占位文件。

## 与引擎层的边界

- **引擎层 C++ 永远在引擎仓库的 `Engine/**`,不随项目分发,也不要复制进这里。**
  `World/`、`WUI/`、`Renderer/` 这些头文件来自引擎构建产物,不是项目文件。
- 项目层 C++ 只写"这个游戏自己的东西":组件、系统、脚本行为。

## 怎么开始

1. 组件加到 `src/Components/`,`WE_SCHEMA` 声明请参考引擎文档 `docs/dev/`。
2. 脚本用编辑器 Scripts 面板的 "New Script" 从 `assets/scripts/templates/WorldScript.lua`
   复制生成,属性面板会按 `WE_FIELD` 声明渲染编辑控件。
3. C++ 脚本用编辑器 Scripts 面板的 `新建 C++ 脚本…`:它落到 `<项目根>/src/Scripts/<名字>.h`,
   并在类型账本 `<项目根>/src/Generated/Game.manifest` 里登记该脚本声明的类型。

## 构建本项目自己的 Game.dll(PROJ-8)

项目自带 `CMakeLists.txt` + `build.cmd`:引擎以**子项目模式**被引用(只构建 World 与本项目的
Game 模块),产物就在本项目里:

```bat
build.cmd                      :: Debug(默认)
build.cmd <引擎根> Release      :: 指定引擎根 / 配置
```

产出 `<项目根>/build/x64-<配置>/bin/<配置>/Game/<配置>/Game.dll`(同目录带
`WorldRuntime.dll`)。`src/Generated/Game/GameSchemaRegistration.*` 由构建按
`src/{Components,Scripts}/*.h` 重新生成;`Game.manifest` 是**双向类型账本**,首次配置时
自动从源码声明派生,此后由人和编辑器维护(声明了没登记 / 登记了没声明都会被构建拒绝)。

构建完回到编辑器(用 `--project <项目根>` 打开的项目),`文件 ▸ 重载 C++ 模块` 即可加载
新的 Game.dll —— 编辑器**优先**加载项目构建出来的这一份。

## 本模板自带的示例 C++

- `Components/SampleDataComponent.h` —— 纯数据组件(8 个字段覆盖 bool/int/float/vector/string)。
- `Scripts/ExampleScript.{h,cpp}` —— C++ 脚本属性的"特性全览"(标量 / 枚举 / 资产 / 嵌套
  struct / Array / Map,含声明默认值与只读摘要行)。

它们跟着项目走,由 `build.cmd` 编进本项目的 Game.dll;构建后 `Game::SampleDataComponent` /
`Game::ExampleScript` 出现在编辑器的组件与脚本列表里。
