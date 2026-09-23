# 命名规范

> 与 [`file-norms.md`](file-norms.md) §5 同源;本页是**可直接照抄的对照表**。

## 1. 目录

| 规则 | 说明 |
| --- | --- |
| 一个目录一个职责,能用一句话说清 | 说不清就拆;每个顶层目录在 `project-layout.md` 有职责行 |
| 目录名 **PascalCase 仅限模块/子系统**(`Engine`、`Editor`、`Renderer`、`RHI`、`WUI`) | 工具/过程目录用小写(`tools`、`vendor`、`third_party`、`projects`、`local`) |
| 内容目录用**复数、全小写**:`scenes/ materials/ models/ prefabs/ textures/ localization/ scripts/` | 单数例外:专有目录名(如 `intermediate/`)必须写进规范 |
| 工具面目录必须**工具中立**(`tools/agents/**`) | 产品专用配置只放根级 dot-dir 薄适配(`.codex/**`、`.claude/**`) |

## 2. 源码文件

| 类别 | 规则 | 例 |
| --- | --- | --- |
| C++ 类/结构 | 文件名 = 类型名(PascalCase),一文件一类型 | `MaterialSurface.cpp` / `MaterialSurfaceRuntime.h` |
| 内部/私有头 | `<Name>Internal.h` | `MaterialSurfaceRuntimeInternal.h` |
| 生成代码 | `<Module>/Schema/Generated/**` 或 `<Module>/Generated/**`,头部标 "Generated … Do not edit" | `Engine/src/World/Schema/Generated/World/WorldSchemaRegistration.cpp` |
| 测试 | `tests/<模块>/<概念>Tests.cpp`,target 名 `World.<概念>`(文件名 ⇒ target 名)** | `tests/World/MaterialTests.cpp` → `World.Material` |
| 着色器(引擎) | `<模块>_<用途>.hlsl`;公共 include 片段用 `.hlsli` | `Renderer3D_Solid.hlsl`、`MaterialSurfaceContract.hlsli` |
| 着色器(用户材质) | `.hlsl` = 材质本身;`.wmat` = 实例 | `shaders/glass.hlsl` |

**例外:文件名用 `<Domain>Tests.cpp`(目标名 `World.<Domain>`)的映射由 `tests/CMakeLists.txt` 维护;
分域后目录=模块、文件名=概念。

## 3. 符号与代码

| 对象 | 规则 |
| --- | --- |
| 类型/函数 | PascalCase;成员 `m_`;常量 `k` 前缀;宏 `WLD_` 前缀;导出宏 `WLD_API` |
| 命名空间 | = 目录路径(`World::Renderer::…`);不新建顶层命名空间 |
| 引擎头 include | 从 include 根写全路径:`#include "World/Renderer/Material.h"`(`<>` 仅用于第三方) |

## 4. 脚本与工具

| 类别 | 规则 | 例 |
| --- | --- | --- |
| PowerShell / Python 脚本 | kebab-case | `check-environment.ps1`、`verify-pixel-baseline.ps1` |
| 一次性探针 | `<批次id>-<主题>-probe.py`,放 `tools/agents/scratch/` | `m4s3-hlsl-live-probe.py` |
| 任务 / 派工 / 报告 | 任务 ID `YYYYMMDD-HHMM-短名`;报告 `reports/<任务ID>-<角色>.md` | `reports/M4-S3-kernel.md` |

## 5. 数据与内容

| 类别 | 规则 |
| --- | --- |
| 引擎自有格式 | 扩展名带 `w`/`we` 前缀,并登记到 [`formats.md`](formats.md) |
| 内容文件 | 全小写 ASCII,词间 `-`;路径用**内容根相对**写法(契约,不得含构建路径/绝对路径) |
| 本地化 | key 点分层、面板前缀;`en.*` 为基准,`zh-CN.*` 只覆盖差异 |
| a11y / 可脚本化 id | 小写点分层,与命令面板 id 同源;不随文案变化 |
