# vendor/tools/slang/ — Slang 着色器编译器（CLI 子集，入库）

引擎唯一的着色器编译器。形态 = **以进程调用的外部 CLI**，按
[`../README.md`](../README.md) 的判据落在 `vendor/tools/<名字>/`。

## 内容（只含运行 `slangc` 必需的两个二进制 + 许可）

| 文件 | 字节 | sha256 |
| --- | --- | --- |
| `bin/slangc.exe` | 276480 | `71562B573B9F4ABA698A660BA041CAEC2F3F5CA4F8F77761E30FCFBACDB029AB` |
| `bin/slang-compiler.dll` | 25873920 | `A0E010523AE49350F13F2B7300860C83073A92C7DEDB59CC869D2C236450403D` |

**明确不含**：`slang-llvm.dll`（80.5MB）、`slang-glslang.dll`（5.9MB）、`slang.dll` /
`slang-rt.dll` / `gfx.dll`、`slangd.exe` / `slangi.exe`（LSP/REPL）、标准模块目录
`bin/slang-standard-module-<版本>/`（30.7MB）。理由：引擎只用 `-target spirv` 直出
SPIR-V（不需要 LLVM JIT、GLSL 文本后端与语言服务器），实测覆盖见下。

## 来源与 pin

- 上游：`shader-slang/slang` release `v2026.18.2`，资产 `slang-2026.18.2-windows-x86_64.zip`
  （`https://github.com/shader-slang/slang/releases/download/v2026.18.2/slang-2026.18.2-windows-x86_64.zip`）
- 源 zip sha256：`747602AEC6B3623658D55FEA87492D71828E15D16802D7941204FDE418CEEE8E`
- 取件 / 升级（幂等）：`tools/agents/update-slang-tool.ps1`
- 版本同步点（升级时一起改）：根 `CMakeLists.txt` 的 `WLD_SLANG_DIR` 默认值、
  `templates/{project-empty,project-example}/CMakeLists.txt`、`tools/agents/check-environment.ps1`
  的 pin 表、`tools/agents/fetch-slang.ps1` 的 pin 表（整包，可选）。
- 许可：Slang 本体 `LICENSE`（Apache-2.0 WITH LLVM-exception）；上游 release 的
  `LICENSES/` 与 `third-party-notices/` 原样随附。升级时由脚本一并覆盖。

## 为什么入库（"重物不入库"的破例记录）

`vendor/README.md` 规则 3 建议 >20MB 只入库 pin + 获取脚本。本条目是**经用户批准的破例**
（2026-09-30，B 方案）：

- **为什么不走 submodule / 源码编译**：Slang 是大型 C++ 工程，源码构建需要其自带依赖链，
  构建时间与复杂度远超收益；
- **为什么不入整包**：整包解压 166MB，其中 `slang-llvm.dll` 80.5MB + 标准模块 30.7MB
  与本引擎用法无关；
- **为什么可以入**：实测最小集就是上表两个文件（共 ~25MiB），覆盖引擎全部 `slangc` 调用面；
- **代价（已知并接受）**：二进制进入 git 历史，不可回收；版本升级 = 换二进制 + 更新 pin 表。

## 实测覆盖的调用面（2026-09-30）

| 用例 | 命令要点 | 结果 |
| --- | --- | --- |
| Vulkan 顶点 | `-target spirv -profile vs_6_0 -fvk-use-entrypoint-name -entry VSMain` | exit 0，产出 `.spv` |
| GL 顶点 | `-target spirv -profile vs_5_0+spirv_1_0 -fvk-use-gl-layout` | exit 0 |
| 像素 + 反射 | `-profile ps_5_0+spirv_1_0 -fvk-use-gl-layout -reflection-json <out.json>` | exit 0，JSON 19KB |
| 版本串 | `slangc -v` | `2026.18.2` |
| 端到端 | `World.ShaderPipeline`（真实引擎/材质着色器 + `-I` 库 include）与 `Editor.exe --cook` | 见任务 `20260930-1000-slang-tool-in-repo` 验证记录 |

## 谁在用

- 根 `CMakeLists.txt` 的 `WLD_SLANG_DIR` 默认值 = `<repo>/vendor/tools/slang/bin`
  （`-D` / 环境变量可覆盖到别的目录，例如仓库外整包）；
- 引擎/编辑器的着色器编译与烘焙都以该宏为**唯一事实源**（`ShaderUtils.cpp` 的
  `SlangcPath()`），打包产物不调用工具。

## 运行时生成物(不入库)

`slangc.exe` 首次需要 GLSL 模块时会在**自身目录**写一份缓存 `bin/slang-glsl-module.bin`
(~1.2MB;上游发布包不含它)。它已按 `vendor/tools/slang/bin/*.bin` 忽略:
删掉后下一次编译会自动重建(2026-09-30 实测:移走 → GL 目标编译 exit 0 → 文件重新出现)。
