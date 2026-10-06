# 开发者文档

面向**改引擎的人**。面向做游戏的人的文档在 [`../user/`](../user/)。一文件一个主题,不堆长文。

## 这里的文件

| 文件 | 内容 |
| --- | --- |
| [build.md](build.md) | 从零配置 → 构建 → 运行 → 跑测试:真实命令、产物路径、常见失败 |
| [architecture.md](architecture.md) | 模块与产物、依赖方向、渲染 / 场景 / 资产 / 脚本的分层 |
| [extension.md](extension.md) | 扩展点:加组件、加资产类型、加面板、从 Game 接入、加脚本绑定 |
| [naming.md](naming.md) | 命名对照表:类型 / 文件 / 目录 / 资产 |
| [file-norms.md](file-norms.md) | 文件与目录归属规范:过程层进 `tools/agents/**`、什么能丢、什么只归档 |
| [project-layout.md](project-layout.md) | 目录布局与模块职责(Engine / Editor / Runtime / projects / tests / vendor) |
| [formats.md](formats.md) | 引擎自有文件格式登记表(先登记再使用) |
| [localization.md](localization.md) | 本地化三层目录、回退链、工具链与门禁 |
| [shader-contract.md](shader-contract.md) | 材质着色器怎么写:表面函数契约、`//! param` 注解、严格类型、组合采样器、槽位表、诊断码、迁移 |
| [texture-import.md](texture-import.md) | 纹理三层:源图 → `.wtex` 资产(唯一设置家)→ `.wtexc` 产物;格式表 / 缓存键 / 剥离口径 |
| [performance-and-data-layout.md](performance-and-data-layout.md) | 数据布局与性能标准:判定四问、类型四分类、成员顺序与填充、跨界双断言与指纹、修复与优化清单 |
| [profiling.md](profiling.md) | 性能/内存分析:两条通道(帧统计常开 / 作用域按需)、环境变量与 prof.* 命令、**数据字典**(人读与 AI 读同一张表)、trace schema 与验证口径 |

用户侧文档在 [`../user/`](../user/);脚本作者的工作区与补全细节在
[`../user/scripting/lua-tooling.md`](../user/scripting/lua-tooling.md)。

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
