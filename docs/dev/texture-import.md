# 纹理导入管线(源图 + 设置 + 烘焙产物)

> 谁在读:写材质/美术规范的人、改渲染或 cook 的人、写工具/脚本的人。
> 状态:2026-09-25 起,P0(契约)+ P1(内核烘焙)已落地;运行时消费 / cooker / 编辑器面板随后。
> 相关代码:`Engine/src/World/Renderer/{TextureImportSettings,TextureArtifact,TextureCompiler}.*`、
> `Engine/src/World/Core/Sha256.*`、`third_party/{bc7enc16,rgbcx}`(取件脚本 `tools/agents/fetch-texture-encoders.ps1`)。

## 1. 三层结构(源文件永远不动)

```
assets/textures/Icon.png          ← 源(画师产物;VCS 里唯一事实源,可以是 png/jpg/jpeg/tga/bmp)
assets/textures/Icon.png.wtex     ← 设置 sidecar(YAML;**持久**设置,任何时刻可改)
cooked/textures/Icon.png.wtexc    ← 烘焙产物(自描述头 + 逐 mip 块数据;进 .pak)
```

- **不存在"用一种新图片格式替代 PNG"**:源格式只是解码输入,运行时格式由烘焙决定。
- 设置**不是**导入向导的一次性选项:编辑器面板 / 文本 / CI 脚本随时可改,改完重烘。
- 删除 sidecar = 回到自动默认(不是错误);产物自带校验信息,能识别"陈旧产物"。

## 2. 源格式

| 格式 | 支持 | 说明 |
| --- | --- | --- |
| `.png` | ✅ | 推荐;支持 alpha;无损 |
| `.jpg` / `.jpeg` | ✅ | 有损、无 alpha ⇒ 只建议 `usage: color`;`normal`/`data` 的块效应会被放大成可见条纹 |
| `.tga` / `.bmp` | ✅ | 无损;仓库体积大只影响仓库,不影响运行时 |
| `.psd` / `.gif` / `.hdr` / `.pnm` | ❌ | stb 能解,但暂不纳入源类型(HDR 需要 RGBA16F 上传路径;PSD 只取合并结果且无色彩管理) |

美术规范:入库前统一到 sRGB PNG(需要 alpha 时 32 位);法线贴图用无损格式。引擎**不做**色彩空间猜测,
色彩管理由美术流程负责。

## 3. `.wtex` 设置(YAML;JSON 是 YAML 子集)

sidecar 路径 = **源完整名 + `.wtex`**(`Icon.png.wtex`、`Icon.jpg.wtex`)。
未知字段 = **错误**(拼错字段名不会静默变成默认行为)。

```yaml
usage: normal          # color | normal | data | hdr | ui
compression: bc7       # auto | none | bc7 | bc5 | bc4 | bc1 | bc3   (auto = 按 usage)
srgb: false            # 缺省由 usage 派生;显式写才覆盖
mipmaps: true          # 缺省:ui 以外都开
mip_filter: gamma-correct   # box | gamma-correct(sRGB 贴图用后者,避免缩小后变暗)
max_size: 0            # 0 = 不限制;>0 = 按最长边等比缩到该值(>=4)
wrap: repeat           # repeat | clamp | mirror
filter: trilinear      # point | bilinear | trilinear
anisotropy: 4          # 1..16(运行时再按设备上限 clamp)
premultiply_alpha: false
flip_y: false          # 内容贴图默认不翻转(UV 原点左上)
```

序列化只写**显式**字段(默认值不落盘),所以手工写的 sidecar 很短。

## 4. 格式选择(compression: auto)

| usage | 格式 | sRGB | mip | 理由 |
| --- | --- | --- | --- | --- |
| `color` | **BC7** | 是 | 是 | 8bpp,质量接近 RGBA8,支持 alpha |
| `normal` | **BC5** | 否 | 是 | RG 二通道 + 着色器重建 Z(质量最好);源图的 R=X、G=Y |
| `data` | **BC4** | 否 | 是 | 单通道 RGTC(AO/粗糙度/金属度);多通道需求请显式 `compression: bc7` |
| `hdr` | `none` | 否 | 是 | v1 无 RGBA16F 上传路径 ⇒ 不压;后续接 BC6H |
| `ui` | `none` | 是 | **否** | 图标/图集要精确像素且体积小 |

显式写 `compression:` 时以显式为准(例:`data` + `bc7` 做多通道数据贴图)。

## 5. 产物格式 `.wtexc`(v1)

固定 336 字节头(小端)+ 连续 mip 数据段:

| 偏移 | 字段 |
| --- | --- |
| 0 | magic `'WLTX'`(u32) |
| 4 | version = 1(u32) |
| 8 | headerSize = 336(u32) |
| 12 | blockFormat(u16):rgba8=0 / bc7=1 / bc5=2 / bc4=3 / bc1=4 / bc3=5 / rgba16f=6 |
| 14 | flags(u16):bit0 = sRGB,bit1 = 预乘 alpha |
| 16 / 18 | width / height(u16,像素) |
| 20..25 | mipCount / wrap / filter / anisotropy / usage(u8) |
| 28 | settingsHash(u64;源自 `TextureImportSettings::Hash()`) |
| 36 | sourceSha256(32B) |
| 68 | dataSize(u64) |
| 76 | mip offsset[16](u64,相对数据段) |
| 204 | mip size[16](u64) |
| 332 | 对齐填充 4B |

数据布局与 API 契约一致:`Rhi::Texture::SetData(data, size, layer, mip)` 逐级上传;块格式的每级字节数 =
`ceil(w/4) * ceil(h/4) * blockBytes`(BC1/BC4 = 8B,BC3/BC5/BC7 = 16B),**不需要**把图补到 4 的倍数。

## 6. 烘焙与缓存

- 内核入口:`TextureCompiler::BakeBytes`(纯函数:同输入两次逐字节一致)/ `BakeFile` / `BakeDirectory`。
- 目录烘焙:`<sourceRoot>/**/*.{png,jpg,jpeg,tga,bmp}` → `<outputRoot>/<逻辑路径>.wtexc`;
  同目录 `<源名>.wtex` 是设置。
- 缓存键 = `sha256(源字节) + settingsHash + 产物版本` ⇒ 改源或改设置都会自动失效;缓存目录可整体删除重建。
- `--cook` 摘要输出 `baked / uptodate / skipped / failed`(`TextureBakeStats`),失败带人话错误。
- 发行包:默认保留源图(开发/热重载用);`--strip-source-textures` 只带 `.wtexc`。

> 落地实况(2026-09-25,M4-TEX P3;实现 `Editor/src/EditorCooker.cpp`):
> - **接线点**:`--cook` 的 step 4d —— 内容已由 `CookPipeline` 复制进 `cooked/cooked/`(step 3)、
>   着色器烘完(step 4)之后,**打包(step 5)之前**。`sourceRoot` = 清单解析出的内容根
>   (`projects/<项目>/assets`),`outputRoot` = `<build>/cooked/cooked` ⇒ 产物落
>   `cooked/<同逻辑路径>.wtexc`。打包按目录遍历,产物自动进 `.pak`(打包格式未改)。
> - **缓存目录** = `build/x64-<配置>/texture-cache/<源sha256>-<设置hash>-v<产物版本>.wtexc`
>   (build 树里,可整体删除重建;与 `intermediate/ShaderCache/` 同一口径)。
> - **日志**:`[tex] baked=N uptodate=N skipped=N failed=N`(每次 cook 一行);
>   `[tex] restored source copies: sources=N sidecars=N failed=N`(见下"源图在场性自愈");
>   开了剥离则多一行 `[tex] strip-source-textures: sources=N artifacts=N removed=N (orphans=N)
>   unattributed=N failed=N`;失败时 `stats.Errors` 逐条 `[tex] <人话>` 打到 ERROR,**cook 退出码非零**。
> - **`--check` 不跑纹理烘焙**(在 step 3 就返回,与着色器烘焙同一口径):检查耗时不变,日志里没有
>   `[tex]` 行;想校验烘焙结果就跑完整 `--cook`。
> - **增量口径**(实测 2026-09-25,`projects/default` 当前 4 张源图;数字随手头内容变化):冷缓存
>   `baked=4`;第二次 cook `baked=0 uptodate=4`;临时加一张 `Icon.png.wtex`(随后逐字节还原)
>   `baked=1 uptodate=3` ⇒ 只重烘改设置的那一张;剥离后的下一次 cook 会打印
>   `restored source copies: sources=4` 把源图带回开发包。
> - **`--strip-source-textures`(默认关)**:内容根里的源图与 `.wtex` sidecar 不进包,只留 `.wtexc`。
>   三遍:①按内容根逐张要求产物在场(有源图没产物 = **报错并保留**源图),再删源图 + sidecar;
>   ②按 cooked 里每张 `.wtexc` 反推源图/sidecar 落点,清"成对孤儿"(源图已删、旧拷贝仍在);
>   ③**审计不删**剩余同扩展名文件 —— 它们是**导入器产物**(如 glTF 导入生成的
>   `textures/<源 stem>_0.png`,材质直接引用),删了会打断材质 ⇒ 只计数 + WARN。
>   实测(2026-09-25):剥离包里**内容根源图 0 张、`.wtex` 0 个**,`.wtexc` 与引擎 `shaders/**`
>   (80 个 SPIR-V)全在;剥离包从发布目录直接跑 Runtime,两个后端都画出场景(0 device lost / VUID)。
> - **源图在场性自愈**:`CookPipeline` 的增量只认 `cook.db.json`,不会发现 cooked 里的文件被删掉,
>   而剥离正是"删掉 cooked 里源图"的合法动作 —— 所以贴图步骤每次 cook 补回**缺失**的内容根
>   源图/sidecar(`restored source copies` 行)。没有这一步,剥离之后的下一次普通 cook 会静默
>   产出"只剩产物"的开发包(与"默认保留源图"的契约相反)。

## 7. 运行时消费(产物优先)

1. 逻辑路径 `<path>`:先找 `<path>.wtexc`(打包态在 `.pak` 里),命中则解头 + 逐 mip 上传,
   **不**解码源图;格式/尺寸/sRGB 全部来自产物头。
2. 没有产物(开发态或未烘焙)⇒ 回退今天的 stb 路径(RGBA8),行为与旧版一致。
3. 产物与设置不一致(源改过但没重烘)⇒ 编辑器打"需重烘"徽标,cook 重烘,运行时校验失败回退源图。
4. 法线贴图:产物格式为 BC5 时,采样后按 `z = sqrt(max(0, 1 - x² - y²))` 重建;源图仍按 `R=X, G=Y` 写。

> 落地实况(2026-09-25,M4-TEX P2):
> - `World/Renderer/TextureData.*`:`LoadTextureAsset(path)` 返回"是否来自产物 + 头 + 逐 mip 数据"
>   (`Bytes` + `Header.Mips`);没有/坏产物一律回退源图(`TextureData` 逐字节不变)。
> - `World/Renderer/MaterialTextureCache.*`:命中产物时按头的格式(Rgba8/Bc7/Bc5/Bc4/Bc1/Bc3)
>   + `MipLevels` 建纹理、逐级 `SetData(..., mip)`;设备不支持 BC / 建纹理失败 ⇒ 记一条警告 +
>   回退源图(不崩)。日志行 `[material] texture '<path>': artifact format=… mips=… srgb=…` 就是
>   "走的产物路径、没有 stb 解码"的证据。
> - OpenGL 块上传走 `glCompressedTextureSubImage2D`(旧 `SetData` 对块格式没有 `dataFormat` 映射,
>   会断言);Vulkan 原有 `vkCmdCopyBufferToImage` 已按 mip 支持块格式。
> - **采样状态(wrap/filter/anisotropy)还没接**:材质贴图共用渲染器的单个采样器
>   (`Renderer3D` 的 `state.MaterialSampler`,按项目 `rendering.anisotropy` 建),per-texture 采样器
>   需要改描述符写入侧;产物头里的值已解析可用,接线点见 `tools/agents/reports/M4-TEX-P2-runtime.md`。
> - BC5 法线的 Z 重建在**包装层**(`MaterialSurface.cpp` 的 PS 模板 + `WE_NORMAL_TEXTURE_BC5` 开关);
>   引擎标准着色器那条路径尚未接(见 `docs/dev/shader-contract.md` §10 与
>   `tools/agents/reports/M4-TEX-P2-runtime.md` 的实测数字)。

## 8. 编辑器交互(Texture Settings)

- 内容浏览器:源图行有"已烘焙 / 需重烘 / 有设置"徽标;右键 `Texture Settings…` / `Reimport` /
  `Reset to Defaults`。
- 面板:usage / compression / sRGB / mips / mip_filter / max_size / wrap / filter / anisotropy /
  premultiply / flipY + 预览 + `Apply(保存 sidecar)`;改字段 → 重烘 → 预览更新(与 `.wmat` 热重载同一套语义)。

## 9. 验证口径

- 单测 `World.TextureImport`(55 号测试):默认派生表 / YAML 解析(未知字段与越界报错)/ 产物往返 /
  **确定性**(同输入两次逐字节一致)/ mip 尺寸链 / 非 4 倍边长 / max_size 缩放 / 目录烘焙缓存命中与失效 /
  SHA-256 标准向量。
- 运行时:双后端(Vulkan / OpenGL)抓图对比(BC7 产物 vs RGBA8 参考,阈值化 maxDiff + 平均误差),
  法线用例单独一张;打包态 `verify-packaged3d.py` 必须仍 PASS。
- 编辑器:AI 通道探针(改设置 → 产物字节变化 → 预览更新 → 夹具逐字节还原)。
