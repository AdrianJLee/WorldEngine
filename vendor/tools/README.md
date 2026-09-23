# vendor/tools/ — 以进程调用的外部 CLI 工具根

按 [`../README.md`](../README.md) 的判据,外部 CLI 工具固定在 `vendor/tools/<名字>/` 下
(自带 `bin/` 有例外;重物默认不入库)。

**当前为空**:Slang-T6b(2026-09-23)删掉了仅有的两个外部工具。着色器工具现在只有
**Slang**,且按"重物不入库"走 FETCH + sha256,落在仓库外的 `WLD_SLANG_DIR`
(默认 `<repo 同级>/WorldEngine-deps/slang-<版本>/bin`,见 [`../README.md`](../README.md)
与 `tools/agents/fetch-slang.ps1`)。

这个目录保留为空目录的占位(布局机检 `tools/agents/check-layout.ps1` 要求 `vendor/tools/` 存在);
将来放小体积 CLI 工具时,直接建 `vendor/tools/<名字>/` 并把条目写进 `../README.md` 的条目表。
