# 项目布局(目标形态与职责)

> 配套:[`file-norms.md`](file-norms.md)(通用文件/命名规范)、[`naming.md`](naming.md)、
> [`formats.md`](formats.md)、[`../../vendor/README.md`](../../vendor/README.md)(第三方判据)。
> 本页是**目标形态**;迁移进度见 §6。

## 1. 顶层目录职责(一句话)

| 目录 | 一句话 | 谁依赖它 |
| --- | --- | --- |
| `Engine/` | 引擎共享库(目标 `World` → 产物 `WorldRuntime.dll`);含平台层、WUI、Schema、生成器与引擎资源 | `Editor` / `Runtime` / `Game` / `tests` 全部 |
| `Editor/` | 编辑器宿主:窗口壳 + 面板 + 编辑器资源 | `Engine` |
| `Runtime/` | 运行宿主:无编辑器启动游戏 | `Engine` |
| `Game/` | gameplay DLL(`Game.dll`):只放项目逻辑源码 | `Engine` |
| `projects/<名>/` | **项目内容根**:`project.we.yaml`、`assets/**`、`levels/**`、项目级 dot 配置 | 运行时/编辑器按清单读取 |
| `tests/<模块>/<概念>Tests.cpp` | 引擎与宿主测试;target 名 `World.<概念>` | `Engine`(部分目标额外编译 Game/TestKit 生成源) |
| `third_party/<名>/` | 参与编译的第三方**源码**(submodule 优先) | 各 target |
| `vendor/tools/<名>/` | 以**进程**调用的外部工具(dxc、spirv-cross …) | 构建期/运行时 |
| `tools/agents/**` | 本机工作流与过程层(方案/派工/报告/scratch);**不入库、可丢** | 人 + AI |
| `local/**` | 本机状态(编辑器偏好/布局/窗口/最近使用);**不入库、可整体删除** | 编辑器 |
| `docs/{user,dev}/**` | 公开文档(用户手册 / 开发者文档) | 人 |
| `build/**` | 一切派生物 | — |

**依赖方向单向**:`Editor/Runtime/Game/tests → Engine → third_party`。反向依赖即违规。

## 2. Engine 内部

```
Engine/
  src/World/**            实现与私有头(命名空间 World::*;include 走 World/...)
    Platform/**           平台层(窗口/输入/系统;唯一 include 根)
    RHI/**                新渲染后端抽象(OpenGL/Vulkan)
    Renderer/**           渲染器/材质/后处理
    WUI/**                自研 UI 工具包
    Schema/Generated/**   生成的反射注册(入库,标"不要手改")
  generators/<名>/        构建期生成器(如 schema-compiler)
  assets/{shaders,textures}/**  跟库发运的运行时资源
  vendor/  →  见 third_party/(迁移已把依赖挪到仓库根)
```

- **一个 target 一个 include 根**:`Engine/src`;include 第一段 = `World/`(命名空间)。目录 `Engine/`
  是模块名,命名空间 `World::` 与产物名 `WorldRuntime.dll` 是**公开契约**,有意与此不同。
- 旧层 `src/World/Platform/OpenGL/**`(Renderer-era)与新 `src/World/RHI/OpenGL/**`(RHI)并存:
  动 GL 前先确认改哪一层;目标是旧层冻结并逐步并入 RHI。

## 3. 资产与状态(三档口径)

| 类别 | 位置 | 入库 | 备注 |
| --- | --- | --- | --- |
| 引擎/宿主自带运行时资源 | `Engine/assets/**`、`Editor/assets/**` | 是 | 引擎 shader、编辑器本地化与图标 |
| 项目内容(内容根) | `projects/<名>/**` | 是(生成物除外) | 资产系统只扫内容根 |
| 本机状态 | `local/**` | 否 | 删除即复位 |

## 4. 生成物三档

| 生成物 | 位置 | 入库 | 规则 |
| --- | --- | --- | --- |
| schema/反射注册 | `Engine/src/World/Schema/Generated/**`、`Game/src/Generated/**` | 是 | 头部标"Generated … Do not edit",有 `--check` 漂移门禁 |
| Lua 存根 | `projects/<名>/assets/scripts/intermediate/**` | 是(受门禁保护) | 编辑器启动会重写;运行时不得写入库副本(见 `docs/dev/file-norms.md` R3 与 S6 结论) |
| 构建产物/日志/抓图 | `build/**` | 否 | 任何源目录里出现 `.lib/.pdb/.obj` 即违规 |

## 5. 第三方与工具

判据、入证据(来源+pin+许可)、重物阈值、patch 规则、**可替换工具**(`WLD_SHADER_TOOL_DIR` /
`WLD_DXC_DIR` / `WLD_SPIRV_CROSS_DIR`)见 [`vendor/README.md`](../../vendor/README.md)。

## 6. 迁移进度(2026-09-23,分支 `codex/layout-engine`)

| # | 迁移 | 状态 |
| --- | --- | --- |
| 1 | `World/` → `Engine/`(submodule/CMake/脚本/文档) | ✅ 已交付(54/54) |
| 2 | 删除未使用 vendored `VMA` | ✅ |
| 3 | `Platform/` → `Engine/src/World/Platform/`(唯一 include 根) | ✅ |
| 4 | 编辑器状态 → `local/`;`Editor/Resource/Icons` → `Editor/assets/icons` | ✅ |
| 5 | `schema-compiler` → `Engine/generators/` | ✅ |
| 6 | `Engine/vendor` → `third_party/`;工具 → `vendor/tools/` + 变量可替换 | ✅ |
| 7 | `tests/` 按模块分域 | ✅ 已交付(54/54,ctest 名单 0 差异) |
| 8 | `Game/` 内容 → `projects/default/`(DLL 只留 `src/`) | ✅ 已交付(54/54、冒烟 OK、机检 exit 0 无 WARN) |
| 9 | 机检脚本 `tools/agents/check-layout.ps1` | ✅ 随本页提交 |
