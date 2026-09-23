# 开发者文档

面向**改引擎的人**。面向做游戏的人的文档在 [`../user/`](../user/)。一文件一个主题,不堆长文。

## 这里的文件

| 文件 | 内容 |
| --- | --- |
| [build.md](build.md) | 从零配置 → 构建 → 运行 → 跑测试:真实命令、产物路径、常见失败 |
| [architecture.md](architecture.md) | 模块与产物、依赖方向、渲染 / 场景 / 资产 / 脚本的分层 |
| [extension.md](extension.md) | 扩展点:加组件、加资产类型、加面板、从 Game 接入、加脚本绑定 |
| [shader-contract.md](shader-contract.md) | 材质着色器怎么写:表面函数契约、`//! param` 注解、严格类型、组合采样器、槽位表、诊断码、迁移 |

## 仓库结构(速查)

| 目录 | 产物 | 是什么 |
| --- | --- | --- |
| `World/` | `WorldRuntime.dll` | 引擎核心(共享库) |
| `Game/` | `Game.dll` | 游戏代码(独立 DLL),通过显式接口向引擎注册 |
| `Editor/` | `Editor.exe` | 编辑器宿主 |
| `Runtime/` | `Runtime.exe` | 游戏运行宿主 |
| `tests/` | 测试套件 | 引擎回归测试(需配置时开 `WORLD_BUILD_SCRIPT_TESTS`) |
| `vendor/`、`third_party/` | 第三方依赖 | 仓库内锁定版本,不要替换成系统版本 |

## 约定

- 改公共接口或用户可见行为时,**同一提交内**更新对应文档。
- 一个文件一个主题;写不下的拆文件,不要堆成一份巨型文档。
- 报告验证结果时分清三件事:**静态检查 / 编译通过 / 实际运行过**;只编译不等于验证过运行时行为。
- 改组件注解要重跑 schema 生成器(`World.SchemaDrift` 会拦);增删源文件要重新配置 CMake。
