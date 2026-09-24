# 文件与目录规范(v0.1,2026-09-23)

> 适用:`WorldEngine` 公开仓库工作树。目标只有一个:**任何文件放在哪、叫什么、能不能丢,不看记忆,
> 只看本页**。v0.1 是提案 + 已生效项;标记「待落地」的需要一次独立提交(见 §9)。

## 1. 三问判据(任何新文件先过这三问)

1. 它是**产物**吗?(能由源码/脚本重建:编译输出、生成的 schema、抓图) → 放 `build/**` 或目录内忽略,
   **不入库**。
2. 它是**过程记录**吗?(任务台账、派工、报告、一次性脚本、抓图) → 放 `tools/agents/**`,**可丢**。
3. 都不是 → 它是**耐久资产**(源码、公开文档、约定、skill/探针、资产):**必须入库**,
   且要落在 §3 表里对应的目录。

## 2. 根目录白名单

根目录**只允许**下列条目;新增根级文件/目录需要评审记录:

```
.github/  .gitignore  .gitmodules  AGENTS.md  CMakeLists.txt  README.md
build/    docs/       Editor/      Game/      Runtime/       tests/
tools/    vendor/     World/
```

- **禁止**在根目录新建游离的过程目录(如 `scratch/`、`tmp/`、`reports/`):一律进 `tools/agents/**`。
- 根级新增"项目内容/示例项目"目录前先读 §7(资产根规则)。

## 3. 目录职责表

| 目录 | 放什么 | 不放什么 | 入库 |
| --- | --- | --- | --- |
| `Engine/src/World/**` | 引擎库(+namespace `World`);`RHI/`、`Renderer/`、`WUI/`、`Scene/`、`Schema/` 等 | 任何宿主/编辑器专用代码 | 是 |
| `Engine/src/Platform/**` | 平台层(窗口/输入/系统工具)。**注意**:`Platform/OpenGL/**` 是旧渲染抽象,与新 `RHI/OpenGL/**` 并存,属于历史包袱(见 §10.2) | 新功能 | 是 |
| `third_party/<name>/` | 编进 `WorldRuntime.dll` 的第三方**源码**(见 `vendor/README.md`) | CLI 工具、预编译库 | submodule 优先 |
| `Editor/src/**` | 编辑器宿主与面板;`WUI/Panels/**` 一文件一面板 | 引擎能力(应下沉到 `World/**`) | 是 |
| `Editor/assets/**` | 编辑器自带资源(本地化、图标、字体) | 项目内容、生成物 | 是 |
| `Game/` | 默认示例项目与 gameplay DLL(`project.we.yaml` + `assets/**` + `src/**`) | 引擎代码 | 是(生成物除外) |
| `Runtime/` | 运行宿主(无编辑器启动游戏) | 业务逻辑 | 是 |
| `tests/**` | 每个领域一个可执行测试(ctest 名 `World.<Domain>`) | 夹具资产(应放 `projects/<名>/assets/**` 或临时目录) | 是 |
| `docs/user/**` `docs/dev/**` | 公开文档:用户手册 / 开发者文档 | 内部过程记录(走 `tools/agents/**`)、私有知识(走 `WorldEngine-docs`) | 是 |
| `tools/agents/**` | 过程层 + 工具面:`tasks/ dispatch/ reports/ scratch/ tmp/ archive/ skills/ multi-agent/ fonts/` | 任何"项目运行需要"的文件 | 见 §9 |
| `vendor/` | 外部工具与记录(见 `vendor/README.md`) | 参与编译的源码(那属于 `third_party/**`) | 见 `vendor/README.md` |
| `build/**` | 全部构建产物、日志、中间缓存 | 源码、文档 | 否 |

## 4. 第三方内容

判据、证据要求、重物阈值、patch 规则与目标布局见 [`vendor/README.md`](../../vendor/README.md);
目标布局(待落地):`third_party/<name>/`(源码)+ `vendor/tools/<name>/`(CLI)+ `vendor/libs/<name>/`(预编译库)。

## 5. 命名约定

| 类别 | 约定 | 示例 |
| --- | --- | --- |
| C++ 源文件/头文件 | PascalCase,与主类型同名;一文件一主题 | `MaterialSurface.cpp` / `MaterialSurfaceRuntime.h` |
| C++ 类型/函数 | PascalCase;成员 `m_`;宏 `WLD_` 前缀;引擎宏 `WLD_API` | `class MaterialSurfaceRuntime` |
| 生成代码 | 落 `Generated/` 子目录,文件头写"由脚本生成,不要手改" | `World/Schema/Generated/…` |
| 引擎 shader | `<模块>_<用途>.slang`;用户材质扩展名 `.slang`,实例 `.wmat` | `Renderer3D_Solid.slang` |
| 资产 | 小写 + 点分层扩展名(`.we.yaml`/`.wmat`/`.wmodel`);内容根相对路径 | `materials/glass.wmat` |
| 构建脚本/PS | kebab-case | `check-environment.ps1`、`verify-pixel-baseline.ps1` |
| Python 探针 | `<批次-id>-<主题>-probe.py`,放 `tools/agents/scratch/` | `m4s3-slang-live-probe.py` |
| 任务/派工/报告 | 任务 ID = `YYYYMMDD-HHMM-短名`;报告 = `reports/<任务ID>-<角色>.md` | `reports/M4-S3-kernel.md` |
| 本地化 key | 点分层,面板/域前缀;英文为基准(源码内联),目录只做覆盖 | `panel.material.shader.compile.ok` |
| 本地化目录 | 层=模块,目录=域,文件=域内件;文件头 `$owns` 声明键前缀;递归扫 `<lang>/**/*.json`,高优先层覆盖 | `Editor/assets/localization/zh-CN/panels/material_editor.json` |
| a11y / 可脚本化 id | 小写点分层,与命令面板 id 同源,稳定不随文案改 | `material.save`、`material.shader.compile.status` |
| 工具面目录(过程层/协作协议/skill) | **工具中立**命名,不出现产品名 | `tools/agents/**`(目标;现状 `tools/agents/**`) |
| 产品专用配置(某工具才读的文件) | 只放**根级 dot-dir 的薄适配层**,内容引用中立文件 | `.codex/agents/*.toml`、`.claude/**`、`.github/copilot-instructions.md` |

## 6. 文件规模与拆分

- 一文件一主题;**超过 ~60KB 需要在评审里说明**,超过 ~100KB 必须给出拆分计划。
- 当前超标(2026-09-23 实测):`MaterialEditorPanel.cpp` 267KB、`EditorShell.cpp` 192KB、
  `ContentBrowserPanel.cpp` 159KB、`WuiWidgets.cpp` 132KB、`PropertiesPanel.cpp` 115KB、
  `Renderer3D.cpp` 102KB。
- 拆分口径建议按"子视图/模式"切(例:材质编辑器 = 预览 / 参数区 / 代码区 / 诊断区 / 工作流动作)。

## 7. 资产根规则

- `<Module>/assets/**` = **该模块自带的运行时资源**(引擎 shader、编辑器本地化与图标)。
- **项目内容根 = 项目目录**(默认 `Game/`,由清单声明);资产系统只扫描内容根。
- 生成物(`.wmodel`、导入贴图等)按现行忽略规则不入库;夹具只提交源文件。

## 8. 过程层(`tools/agents/**`)规则

- `tasks/<任务ID>/{plan.md,plan-*.md,tasks/}`:方案与子任务;**一个阶段一份方案**。
- `dispatch/<角色名>.md`:单槽 inbox;`dispatch/archive/` 收已收尾派工单;`reports/<任务ID>-*.md` 报告。
- `scratch/`、`tmp/`:一次性脚本与证据,可随时清;`archive/`:归档,不删。
- 收尾纪律:派工单归档、报告只留结论段、一次性脚本按需清理。

## 9. 入库与忽略(`.gitignore`)

**用户口径(2026-09-23 拍板):`tools/**` 一律不入库。** 公开可看的内容只在 `docs/` 出口;
内部耐久知识去私有库;`tools/**` 只承担"本机过程层"。

**原则:忽略产物,不忽略目录;耐久内容不放 `tools/`。**

- 已修(2026-09-23,`codex/slang-spike` 分支):全局裸名 `tools` → **`/tools`**(裸名会在任意深度命中,
  `World/tools/`、`vendor/tools/` 这类同名目录会被静默吞掉);新增 `!vendor/tools/**/bin/` 工具目录例外;
  `vendor/slang/`(实验包,166MB)显式不入库。
- **内容去向三选一(没有第四种)**:
  1. 想公开、想被 clone 到 → `docs/user/**` 或 `docs/dev/**`(唯一公开文档出口,入库);
  2. 内部耐久(协议、工作法、不变量、脚本)→ 私有库 `WorldEngine-docs`(`knowledge/**`、`workflow/**`,
     或新增 `tooling/**`),入库到私有库;
  3. 一次性的过程记录(任务台账、派工、报告、抓图、临时脚本)→ `tools/**`,**不入库、可丢**。
- 由此得出的纪律:**耐久的东西不许只留在 `tools/`**。现在放在 `tools/agents/` 的
  `skills/**`、`multi-agent/**`、`check-environment.ps1`、探针脚本属于第 2 类(耐久),
  建议迁到私有库工具目录,`tools/` 只留第 3 类(见 R2)。
- `.gitignore` 现状:根级 `tools/`、`.codex/` 保持忽略;**`docs/` 保持可见**(公开文档要能入库);
  `AGENTS.md` 与 `.github/` 当前也被忽略 —— 这两项是否入库见 §11 待决项。
- 产物规则保留:`build/`、`bin/`+`bin-int/`、`*.vcxproj*`、`Editor/wui-*.json`、`Editor/editor-prefs.json`、
  资产生成物逐条忽略。

## 10. 跨工具中立性(重要)

**三层结构,越往下越"产品专用":**

1. **根 `AGENTS.md`** = 跨工具事实标准(Codex / Claude Code / Cursor / Copilot / Gemini 等都读它)。
   项目指引写这里,不写产品名。
2. **`tools/agents/**`** = 工具中立的工具面与协议:`tasks/`(方案台账)、`dispatch/`(按角色寻址的单槽派工)、
   `reports/`(报告)、`skills/`(项目工作法)、`multi-agent/`(协作协议)、`check-environment.ps1`、探针。
   协议正文只描述**能力需求**(如"支持子 agent 派遣""能等待并收取结果"),不写 `spawn_agent` 这类具体 API 名。
3. **`.<tool>/**`** = 薄适配层:把中立协议映射到某个产品的配置(如 `.codex/agents/<角色>.toml` 里只写
   "读 `tools/agents/dispatch/<角色名>.md`"这类指针,不复制规则正文)。

**原问题**:目录名 `tools/codex/` 把中立内容绑死在 Codex 上(其他工具/人类看到会以为与自己无关),
而 `AGENTS.md` 里"仓库内维护版本是 `tools/codex/skills/...`"又把产品名写进了项目指引。

**R1(目录改名,纯本机,不影响仓库)—— ✅ 已完成 2026-09-23**:`tools/codex/` → **`tools/agents/`**
(与根 `AGENTS.md` 对称)。活引用已批量替换:`AGENTS.md`、4 个 `SKILL.md` 与脚本/参考、`multi-agent/**`、
`.codex/agents/*.toml` 指针、三处代码注释、`docs/**`、`vendor/README.md`——脚本
`tools/agents/scratch/rename-codex-to-agents.py`,56 文件 / 180 处;`tmp/**` 的 19 个一次性脚本
已归档到 `tools/agents/archive/tmp-20260923/`。

**R2(耐久内容迁出 `tools/`)**:把 `skills/**`、`multi-agent/**`、`check-environment.ps1`、探针脚本
移到私有库(`WorldEngine-docs/tooling/**`),`tools/` 只留 `tasks/ dispatch/ reports/ scratch/ tmp/ archive/`。

**执行清单(R1+R2 一次做,现在最便宜——该目录尚未入库)**:

- 全量替换**活引用**:`AGENTS.md`、skill 的 SKILL.md 与脚本自引用、multi-agent 协议正文、
  `.codex/agents/*.toml` 指针、三处代码注释(`Editor/src/AiControl/AiControlServer.h`、
  `Engine/src/World/WUI/WuiWidgets.cpp`、`Engine/src/World/Script/Sandbox.h`)、私有库
  (`scripts/export-state.ps1`、`knowledge/00-KNOWLEDGE.md`、`generated/STATE.md`、`workflow/**`)。
- **历史记录不改**:`tools/agents/dispatch/{archive,reports}/**`、`tasks/**` 里的旧路径保持原样,
  在新 `README.md` 注明"2026-09-23 前的记录可能引用 `tools/codex/`"。
- 检查清单:`rg -n "tools[/\\\\]codex"` 归零;`.codex/agents/*.toml` 里只留指针;
  `.gitignore` 的 `/tools` 规则维持;`tools/agents/README.md` 写明"任何 AI 工具或人类都按本协议工作"。
- 迁移后 `tools/agents` 不再出现(含私有库)。

## 11. 已知结构性问题(记录,不在 v0.1 修)

1. 引擎代码有两个 include 根:`Engine/src/World/**` 与 `Engine/src/Platform/**`(后者含 `Windows/` 与旧
   `OpenGL/`)。约定:新代码只进 `Engine/src/World/**`。
2. `Platform/OpenGL/**`(旧渲染抽象,仍被 `Renderer/Buffer.cpp` 等 include)与 `RHI/OpenGL/**`(新后端)
   并存:动 GL 前先确认改的是哪一层;目标是旧层冻结并逐步迁入 RHI。
3. `Editor/` 里存在被忽略的 in-source 构建残留(`Editor.vcxproj*`);构建永远在 `build/**` 里做。
4. `Game/` 同时承担"默认示例项目内容"与"gameplay DLL"两重身份,后续拆成项目建设议单列。
5. `tests/**` 54 个可执行缺"改动面 → 该跑哪套"的机器可读映射,建议 `tests/SUITES.md`。

### 待决(需用户定,2026-09-23)

| 项 | 选项 | 影响 |
| --- | --- | --- |
| `AGENTS.md` 是否入库 | (a) 继续本地-only(与 `tools/**` 同策略,靠私有库快照)(b) 入库,让 clone 即有跨工具指引 | (b) 会把"项目指引"公开;(a) 丢机器时只靠快照恢复 |
| `.github/**` 是否入库 | (a) 继续忽略(当前无 CI)(b) 入库并接管 CI | 直接入库即公开工作流定义 |
| 耐久工具面迁私有库(R2) | 迁 `skills/`+`multi-agent/`+`check-environment.ps1`+探针到 `WorldEngine-docs/tooling/**` | 迁完 `tools/` 纯过程层,可随时清 |

### R3(建议尽快做):补 `.gitattributes`(行尾陷阱,已实测)

仓库没有 `.gitattributes`,而本机 `core.autocrlf` 生效:签出会写 CRLF,`git add` 又转回 LF。
后果:`tests/World/ScriptWorkflowTests.cpp` 的"存根漂移门禁"做**逐字节比较**
(`projects/default/assets/scripts/intermediate/WorldEngineAPI.luau`),工作树一旦是 CRLF 就**误报失败**
(2026-09-23 实测:内容逐行相同、仅 CRLF 差异 → 门禁失败)。

建议:新增 `.gitattributes`,至少 `* text=auto eol=lf` + `*.luau text eol=lf`,并给该门禁加
"忽略行尾"或"先规范化再比较"的兜底(否则新人 clone 第一次跑测试就会红)。

## 12. 本规范的变更方式

- v0.1 = 提案;§9 的 `.gitignore` 白名单化落地后升 v1.0。
- 新条目:先在 `docs/dev/file-norms.md` 加一行,再谈自动化(§12)。
- 引用关系:第三方细节见 `vendor/README.md`;知识层(L1)约定见私有库 `WorldEngine-docs/knowledge/**`。

## 13. 可机检项(建议后续加进 CI / 预检脚本)

| 检查 | 判据 |
| --- | --- |
| 未引用源文件 | `Engine/src/**` 下无任何 include 命中且无 CMake 引用 → 报错(如 `Platform/OpenGL` 类) |
| 文件规模 | 单文件 >60KB 警告,>100KB 报错(白名单可豁免) |
| 命名正则 | 按 §5 表逐类校验新增文件名 |
| ignore 漂移 | `git check-ignore` 命中"必须入库清单"任一项 → 报错 |
| 孤儿资产 | 内容根内不被任何 `.wmat`/场景引用且无 `.meta` 记录 → 报告 |
| 产品名泄漏 | **入库范围**内(`docs/**`、`AGENTS.md`)出现具体 AI 产品目录名 → 报错(`tools/**` 本机自用,不检查) |
