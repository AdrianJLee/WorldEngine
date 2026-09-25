# rgbcx(上游来源)

- 仓库:https://github.com/richgel999/bc7enc_rdo(取 `rgbcx` 部分)
- commit:`b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`
- 文件与 sha256:

| 文件 | sha256 |
| --- | --- |
| `rgbcx.h` | `111BD4C0211ECA172BEC9D757E2E597A47C4E4731BB122C9E8761DCA85711BBA` |
| `rgbcx.cpp` | `059243CE8E8E3EC634BC1E3DA3996115AAF36196A8D75A24D3DBB3C8BFE0CA98` |
| `rgbcx_table4.h` | `5051CF4CE17CB1F9D011EF788B7C8D174F2270532DA0514C9F394830BAB72F36` |
| `rgbcx_table4_small.h` | `D484802656FB7F2249674263BBB14399AB9CCB2A997AC0030E51B17A4BF8595F` |
| `LICENSE` | `B3E843763E8D3BDEB0F469ECD4107FC32B5FA27EBEE1AF10D2066BC339292800` |

- 许可:MIT 或 public domain(见 `LICENSE`)。
- 用途:M4-TEX 纹理烘焙的 BC4 / BC5(RGTC)与备用 BC1 / BC3 编码;同时提供 `unpack_bc4/5` 供测试做质量校验。
- 取件/校验:仓库内 `tools/agents/fetch-texture-encoders.ps1`(幂等,sha256 不符即失败)。
- 更新方式:改 `fetch-texture-encoders.ps1` 的 pin 表(commit + 每个文件的 sha256),重跑脚本,并更新本文件。
