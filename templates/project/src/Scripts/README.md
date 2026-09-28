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
3. 本项目独立 `Game.dll` 的构建(项目自带 CMake/VS 工程)是后续能力;当前先用引擎
   仓库的开发构建跑起来(见引擎文档 `docs/user/projects/README.md`)。
