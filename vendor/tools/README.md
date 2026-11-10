# vendor/tools/ — 以进程调用的外部 CLI 工具根

按 [`../README.md`](../README.md) 的判据,外部 CLI 工具固定在 `vendor/tools/<名字>/` 下
(自带 `bin/` 有例外;重物默认不入库)。

**当前条目**:只有一个 —— [`slang/`](slang/README.md)(Slang 着色器编译器 CLI 子集,
~25MiB,入库;2026-09-30 用户批准的"重物不入库"破例,理由与实测覆盖写在条目 README)。
Slang-T6b(2026-09-23)删掉的另外两个外部工具不再存在。

构建期 `WLD_SLANG_DIR` 的默认值指向 [`slang/bin`](slang/bin)(`-D`/环境变量仍可覆盖到
仓库外目录);整包(含 `slangd`/LSP)仍可选走 `tools/agents/fetch-slang.ps1` 落到仓库外。

布局机检 `tools/agents/check-layout.ps1` 要求 `vendor/tools/` 与 `third_party/` 分层存在;
新工具直接建 `vendor/tools/<名字>/` 并把条目写进 `../README.md` 的条目表。
