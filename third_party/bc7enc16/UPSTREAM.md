# bc7enc16(上游来源)

- 仓库:https://github.com/richgel999/bc7enc16
- commit:`b37c4bb65af406ab2a1071f2a312bdbe112e4f53`
- 文件与 sha256:

| 文件 | sha256 |
| --- | --- |
| `bc7enc16.h` | `D71955C059667F9828C89609CC0293F8183BC28985C60A1719C0FAF623ECE4A0` |
| `bc7enc16.c` | `2103CF1A67F5FED24F4AEA95E9E220E6792F88231CAA214FAA235CF2C6E15B10` |
| `LICENSE` | `0156F294F430EC7BB63726F70F84DA24FE3288E060DECB04B6370FB9E1699A98` |

- 许可:MIT 或 public domain(见 `LICENSE`)。
- 用途:M4-TEX 纹理烘焙的 BC7 编码(`bc7enc16_compress_block`)。
- 取件/校验:仓库内 `tools/agents/fetch-texture-encoders.ps1`(幂等,sha256 不符即失败)。
- 更新方式:改 `fetch-texture-encoders.ps1` 的 pin 表(commit + 每个文件的 sha256),重跑脚本,并更新本文件。
