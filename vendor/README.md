# vendor/ 的判据(第三方内容怎么放)

本目录与 `third_party/`(原 `World/vendor/` → `Engine/vendor/`)的划分**只看"交付形态"**,不看"谁在用"。任何第三方条目按三选一落位:

| 形态 | 放哪 | 入库策略 |
| --- | --- | --- |
| **参与编译的源码**(静态/动态库源码) | `third_party/<name>/` | submodule 优先(仓库只存 commit);不能 submodule 的才 vendored 源码,必须写清版本 |
| **以进程调用的外部工具**(CLI) | `vendor/tools/<name>/` | 小 CLI(≤ 数 MB)+ LICENSE 可入库;`bin/` 里的东西按需(见 `.gitignore` 例外规则) |
| **无法源码构建的预编译库** | `vendor/libs/<name>/` | 默认**不入库**:pin 版本 + sha256 + 获取脚本;临时的放 `tools/agents/scratch/` |

## 四条硬规则

1. **一份内容只有一个位置,一个名字只指一个东西。** 曾经的同名不同物(2026-09-23 已按本判据分开):
   **CLI** 与**库**同名不同物时会分居 `vendor/tools/<名>/` 与 `third_party/<名>/`。改之前先确认
   自己动的是哪一个。
2. **每个条目自带三件证据**:来源 + pin + 许可。submodule 用 commit 钉;非 submodule 用
   `版本号 + sha256 + 获取方式`(写在本目录或条目内 `FETCH.md`)。
3. **重物不入库**:二进制/超大包(建议阈值 >20MB)只入库"pin 记录 + 获取脚本 + LICENSE"。
   `vendor/` 里出现新的大包时,先在评审里回答"为什么不走 submodule / 为什么不能构建"。
4. **第三方目录只读**:要打补丁就放 `patches/NNNN-描述.patch` + 应用脚本,**不直接改 vendor 本体**;
   项目的适配代码写在项目自己的层(如 `Engine/src/World/**`)。

## 当前条目(判据示例)

| 条目 | 形态 | 入库内容 | 备注 |
| --- | --- | --- | --- |
| (无) | — | — | **Slang-T6b(2026-09-23)后只有 Slang**:以前的两个着色器 CLI 工具与其中一个的库形态(工具目录 + submodule)已全部删除 |
| `vendor/slang/` | 工具(实验,**已删**) | 整包不入库;获取方式见 [`tools/agents/scratch/slang-spike/FETCH.md`](../tools/agents/scratch/slang-spike/FETCH.md) | Spike 收尾后按"重物不入库"清掉(166MB);采纳时二选一:只入库 CLI 子集,或 FETCH + 版本/sha256 脚本 |

## 与 `.gitignore` 的配合

### 着色器工具:只有 Slang(重要)

着色器编译只有一条路:根 CMake 的 `WLD_SLANG_DIR`(= **含 `slangc.exe` 的目录**,`-D` 可覆盖,
默认 `<repo 同级>/WorldEngine-deps/slang-<版本>/bin`),由
[`tools/agents/fetch-slang.ps1`](../tools/agents/fetch-slang.ps1) 按固定版本 + sha256 落盘;
运行时代码只读这个编译期常量,没有 env / vendor / PATH 兜底猜测。旧的着色器编译/转译工具
(整包工具目录与对应的库 submodule)已删除,GL 目标也走 Slang 直出的 SPIR-V(GL 4.6 + `GL_ARB_gl_spirv`)。

新工具按 `vendor/tools/<name>/` 落位;若它同时提供"库"形态,库走 `third_party/<name>/`,不要同名同层。

- `bin/`、`bin-int/` 是**构建产物**规则(挡住 in-source 构建产物,例如 `third_party/Glad/bin/*.lib`)。
- `!vendor/tools/**/bin/` 是**工具目录例外**:工具包自带的 `bin/`(CLI 本体)允许入库。
- 新第三方包若自带 `bin/`,先判断它属于"工具"(开例外)还是"构建产物"(保持忽略),不要靠改全局规则解决。
