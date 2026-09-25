# 材质着色器契约(Slang 严格子集)

> 谁在读:写 `.slang` 材质着色器的人 / AI 生成代码。
> 工具链:Slang 是**唯一**着色器编译器(ADR `0003-slang-first-gl-spirv`);
> `.slang`(Slang 源;**HLSL 语法是它的子集**)→ SPIR-V → { Vulkan, OpenGL 4.6 + `GL_ARB_gl_spirv` },
> 每个后端一份 permutation。材质着色器资产的扩展名**只有 `.slang`** —— 导入、编译、烘焙、
> 内容浏览器向导都按它走,`.hlsl` 不是资产类型(旧文件按未知扩展名处理)。
> **用户面是破坏性的**:HLSL 隐式转换从 warning 变 error;`Texture2D + SamplerState` 要改成组合采样器。
> 相关决策与证据:`tools/agents/tasks/20260923-1700-slang-refactor/plan.md`(v3.1 §12/§13)、
> `tools/agents/reports/Slang-T1-gl-spirv.md`、`Slang-T3-kernel.md`。
>
> **完整示例(可直接在编辑器里打开)**:[`projects/default/assets/shaders/examples/ShowcaseMaterial.slang`](../../projects/default/assets/shaders/examples/ShowcaseMaterial.slang)
> —— 覆盖全部注解类型与修饰符、组合采样器、切线空间法线、参数驱动分支、以及 Slang 的
> `interface` + 泛型约束写法;该文件已在双目标上实编译验证(Vulkan/GL 各 `spirv-val` = 0,GL 模块 SPIR-V 1.0)。
>
> **两条硬性写法**(踩过才写在这里):① **注解行必须独占一行** —— `//! param …` 行尾不能再跟 `// 注释`
> (要解释某个参数,用注解里的 `doc("…")`,它会在参数行悬停与读屏里显示);
> ② `[min,max]` **只适用于 Float/Int**(写在 Vec2/Vec3/Color 上会解析失败)。
>
> **一条渲染陷阱**:`Texture2D` 参数为空(`""`)或路径不存在时,引擎绑定的是**白色 1×1 兜底贴图**。
> 把它当**法线贴图**用会得到 `texel*2-1 = (1,1,1)` 的退化法线 → 光照几乎为 0(看起来**全黑**);
> 法线贴图请指向真实贴图(仓库里有 `textures/flat_normal.png` = 平面法线),或把开关关掉。

## 1. 一个材质着色器长什么样

新建 `.slang` 时照这个骨架写(改注解与 `Evaluate()` 里的赋值即可;下面代码块的语法分类是 HLSL 家族,
Slang 不需要任何语法改写):

```hlsl
//! param Color Tint = 1, 1, 1, 1 group("Appearance") label("Tint") doc("Base tint color")
//! param Float Roughness = 0.5 [0,1] group("Appearance")
//! param Texture2D Albedo = "textures/Icon.png" group("Appearance")

Surface Evaluate(MaterialInputs input)
{
    Surface surface = MakeDefaultSurface();
    surface.BaseColor = Tint.rgb * Albedo.Sample(input.UV).rgb;
    surface.Roughness = Roughness;
    return surface;
}
```

- 文件里**只写** `Evaluate()` 和它用到的辅助函数/常量。顶点、实例化、蒙皮、阴影、光照、
  雾、最终显示空间编码都由引擎包装层提供(模板见 `Engine/src/World/Renderer/MaterialSurface.cpp`)。
- `Surface surface = MakeDefaultSurface();` 先取引擎默认表面,再只改这份材质需要的字段。
- 参数来自 `//! param` 注解,**注解是唯一事实源**:编辑器参数行、cbuffer 布局、运行时上传都由它派生。
- 贴图**不要**在源码里声明 `Texture2D`/`SamplerState`:注解顺序决定槽位,包装层生成
  `[[vk::binding(4 + n, 2)]] Sampler2D <名字>;`,采样写 `<名字>.Sample(uv)`。
- 引擎 uniform(`u_` 开头,如 `u_BaseColor`、`u_AlbedoTexture`)由包装层声明;用户源里只读,
  不要自己声明(`u_` 前缀也是注解参数名的保留前缀)。
- 可复用的纯函数不要复制粘贴:放进 `assets/shaders/lib/<名字>.slang`,材质侧
  `#include "lib/<名字>.slang"` 后直接调用(见 §9「材质函数」)。

## 2. 表面函数契约

函数签名固定:`Surface Evaluate(MaterialInputs input)`。字段表是唯一事实源,定义在
`Engine/src/World/Renderer/MaterialSurfaceContract.hlsli`(X-macro;**共享头片段**,Slang 与 C++ 共用;
扩展名保持 `.hlsli` —— 它不是一个可独立编译的源文件,引擎把它作为文本内嵌进生成的包装源码)。

| `MaterialInputs` | 类型 | 语义 |
| --- | --- | --- |
| `UV` | float2 | 网格 UV0(顶点流) |
| `WorldPosition` | float3 | 插值后的世界坐标 |
| `WorldNormal` | float3 | 归一化世界法线 |
| `WorldTangent` | float3 | 屏幕空间导数构造的世界切线 |
| `WorldBitangent` | float3 | `cross(WorldNormal, WorldTangent)` |

| `Surface` | 类型 | 语义 | 默认(引擎标准着色器) |
| --- | --- | --- | --- |
| `BaseColor` | float3 | **线性** RGB 基础色 | `WeLinearizeColor(u_BaseColor.rgb)` |
| `Metallic` | float | 0..1 | `saturate(u_MetallicRoughness.x)` |
| `Roughness` | float | 0..1 | `saturate(u_MetallicRoughness.y)` |
| `Emissive` | float3 | **线性** RGB 自发光 | `WeLinearizeColor(u_Emissive.rgb)` |
| `Normal` | float3 | 切线空间扰动;`(0,0,1)` = 保留引擎采样到的法线 | `WeDefaultNormal()` |
| `Opacity` | float | 0..1,输出 alpha | `saturate(u_BaseColor.a)` |
| `AmbientOcclusion` | float | 0..1 | `1.0f` |

引擎在 PS 里的顺序是:构造 `MaterialInputs` → `Evaluate()` → 解析着色法线(法线贴图 + `Surface.Normal`)
→ 光照/阴影 → 雾 → `pow(saturate(lit), 1/2.2)` 写颜色、`saturate(surface.Opacity)` 写 alpha。
所以:**颜色写线性**,显示空间编码不要自己做;标量在参与光照前会被 `saturate`,但不要依赖它兜底坏值。

## 3. 严格类型(最常见的一类报错)

Slang 不做隐式宽度转换 —— 隐式窄化/加宽都是 **error**:

| 旧写法 | Slang 诊断 | 修法 |
| --- | --- | --- |
| `float3 v = color4;` / `return color4;`(函数返回 float3) | `E30019 type mismatch in expression`(expected `vector<float,3>`, got `vector<float,4>`) | 显式截断:`color4.xyz` / `.rgb` |
| `float3v += color4;` | `E39999 no overload for '+=' applicable to arguments of type (vector<float,3>, vector<float,4>)` | `float3v += color4.xyz;` |
| `double` 参与 float 运算 | 窄化不再隐式 | 显式 `(float)` 转换 |

其它约定:构造用显式类型(`float3(1, 2, 3)`),不要靠隐式广播;标量进向量也用显式构造。
仓库内 6 个引擎 shader 与包装模板已经是严格形态,迁移脚本见 §8。

## 4. 组合采样器(硬约束)

```hlsl
// 引擎 shader 手写这行;材质着色器由 //! param Texture2D 注解生成
[[vk::binding(1, 2)]] Sampler2D u_AlbedoTexture;

float3 albedo = u_AlbedoTexture.Sample(uv).rgb;          // 只有 uv,没有 sampler 参数
float  depth  = u_ShadowMap.SampleLevel(uv, 0.0f).r;      // SampleLevel / SampleBias 同理
```

- **禁止**`Texture2D + SamplerState` 两变量写法,也**禁止**`[[vk::combinedImageSampler]]`(属性会被忽略)。
- 原因(实测硬约束):`GL_ARB_gl_spirv` 不接受分离的 `OpTypeSampler`;分离形态在本机 NVIDIA 驱动
  首次采样 **14/14 崩溃**(`nvoglv64+0x75abcb`,0xC0000005);组合 `Sampler2D` 同时喂 Vulkan 的
  `COMBINED_IMAGE_SAMPLER` 与 GL 的 `OpTypeSampledImage`,双后端抓图 `maxDiff=0`。
- `.Load(...)` 本来就不带采样器;`.Gather(...)` 在组合采样器形态里同样只传坐标(不传 sampler)。

## 5. 绑定与参数槽位表

材质包装层(用户源不写这些声明,这里是为了对齐 C++ 侧):

| set | binding | 声明 | 内容 |
| --- | --- | --- | --- |
| 0 | 0 | `cbuffer CameraUniforms` | `u_ViewProjection` |
| 0 | 2 | `cbuffer LightUniforms` | 阴影矩阵/参数、`u_Ambient`、`u_LightCounts`、`u_Lights[8]` |
| 0 | 3 | `Sampler2D u_ShadowMap` | 方向光阴影图 |
| 1 | 1 | `cbuffer ObjectUniforms` | `u_Model`、`u_BaseColor`、`u_MetallicRoughness`、`u_Emissive`、`u_Flags`、`u_EntityId` |
| 1 | 3 | `cbuffer BoneUniforms` | `u_Bones[128]` |
| 1 | 4 | `cbuffer MaterialParams` | **注解生成**的参数块(偏移来自 SPIR-V 反射) |
| 2 | 1 | `Sampler2D u_AlbedoTexture` | 材质 albedo |
| 2 | 2 | `Sampler2D u_NormalTexture` | 材质法线 |
| 2 | 4..11 | `Sampler2D <注解参数名>` | 材质贴图参数,按注解顺序连续占位,最多 **8** 张 |

GL 口径:描述符绑定单元 = `binding`(**忽略 set**),所以 UBO 单元占用表是
`0=相机、1=物体、2=灯光、3=骨骼、4=材质参数` —— 新加 UBO 必须避开这五个;贴图走 GL 纹理单元,
与 UBO 不同命名空间。

**法线贴图的格式约定(M4-TEX P2,2026-09-25)**:材质法线槽(`u_NormalTexture`,set 2 binding 2)
可以是 RGBA8 源图,也可以是烘焙产物 `.wtexc` 里的 **BC5(RGTC2)**。BC5 只存 **`R=X` / `G=Y`**,
包装层按 `z = sqrt(saturate(1 - x² - y²))` 重建 Z(与 UE / Unity 的切线空间法线约定一致)。
重建由包装层的编译期开关 `WE_NORMAL_TEXTURE_BC5` 控制:排列键里带 `normal-bc5` 记号即打开
(`WLD_SURFACE_NORMAL_BC5=1` 是诊断覆盖,用于同一材质开/关的 A/B 抓图);默认 `0`
= 与 RGBA8 时代**完全相同**的采样路径(块数据不解码到 CPU,重建必须在着色器里做)。
实现见 `Engine/src/World/Renderer/MaterialSurface.cpp` 的 `ResolveShadingNormal`。
BC5 打开时重建出的 Z **参与合并**(`z = surface.Normal.z * z_reconstructed`;默认权重 1);
而 RGBA8 的既有实现在合并时把 z 直接写成 `surface.Normal.z`(默认 1)= **忽略采样到的 Z**
(法线被压平)。这是 P2 之前就有的行为,按验收口径**未改动** —— 也就是说:
同一张法线贴图写成 RGBA8 与烘成 BC5,在包装层里的 Z 口径目前不一致;
统一(把 RGBA8 也改成用采样 Z)会改变既有输出,属于新范围,需要单独确认。

## 6. `//! param` 注解语法

```
//! param <type> <name> = <default> [min,max] unit("…") group("…") label("…") doc("…")
```

- `<type>`:`Float` / `Vec2` / `Vec3` / `Vec4` / `Color` / `Int` / `Bool` / `Texture2D`(大小写敏感)。
- `<default>` **必填**:数值是逗号分隔字面量(`0.25`、`1, 0.5, 0.25, 1`);`Bool` 是 `true/false`;
  `Texture2D` 是内容根相对路径(`""` = 无贴图)。
- `[min,max]` 只对 `Float`/`Int` 有效(默认 `[0,1]`),默认值必须落在区间内。
- `unit/group/label/doc` 是双引号字符串,可空;每条注解里每个字段最多出现一次。
- `doc("…")`:这个参数的**说明**,给读的人看 —— 编辑器参数行的悬停 tooltip 与无障碍/读屏
  文本都用它(空 = 不显示说明);`doc` 里可以有空格,也可以与其它字段任意顺序混排。
- 参数名:合法标识符,**不能以 `u_` 开头**、不能与模板标识符冲突(`MaterialParams`、`PSMain`、`input`、`surface` …)。
- 注解顺序 = 贴图槽顺序(t4、t5…);第 9 张 `Texture2D` 会被编译器直接拒绝。
- 解析失败给 `行:列: 原因`(1 基,列指向出错 token);注解错误只报错、不写盘,原文件不变。

## 7. 诊断码

编辑器代码列底部逐条显示 `line L:C  <severity>[<code>]: <message>`(可点击跳行);
两个后端是同一份包装源码经同一个 `slangc` 编译(目标 profile 不同),稳定诊断码一致;
已知的后端差异只有 GL 目标 `_5_0+spirv_1_0` profile 会额外打 `E50011` 提示(见下)。

| 码 | 级别 | 含义 | 常见修法 |
| --- | --- | --- | --- |
| `E30019` | error | type mismatch in expression | 补 `.xyz` / `.rgb`,或显式构造 |
| `E39999` | error | 没有匹配的重载(如 `'+='` 的 `float3`+`float4`) | 显式截断,或统一宽度 |
| `E20002` | error | 语法错误(行:列指向出错 token) | 按行号修语法 |
| `E39029` | warning | `register(...)` 没有对应的 Vulkan binding | 改 `[[vk::binding(N,S)]]` |
| `E39001` | warning | 显式绑定重叠 | 换 binding 号(见 §5 占用表) |
| `E31000` | warning | 未知属性(旧属性残留,如 `combinedImageSampler`) | 删属性,用组合采样器 |
| `E50011` | warning | SPIR-V version too old | GL 目标**必须**是 SPIR-V 1.0,这条是预期提示,不是错误 |

## 8. 从旧写法迁移

| 旧写法(旧编译器) | 新(Slang) |
| --- | --- |
| `Texture2D Albedo; SamplerState AlbedoSampler;`(或 `[[vk::combinedImageSampler]]`) | `Sampler2D Albedo;`(材质参数走注解,不要手写声明) |
| `Albedo.Sample(AlbedoSampler, uv)` | `Albedo.Sample(uv)` |
| `cbuffer X : register(b2)` | `[[vk::binding(2, 0)]] cbuffer X` |
| `float3 r = color4;` | `float3 r = color4.xyz;` |

**无需迁移(2026-09-23)**:`.hlsl` 作为材质着色器扩展名已整体废除,仓库与示例项目都没有应用在用
(决策见 `tools/agents/tasks/20260923-2200-slang-native/plan.md`),也没有可跑的迁移脚本 ——
上表的方言差异只用于**手工**把旧写法改到 `.slang`。`.hlsli` 共享头片段
(`MaterialSurfaceContract.hlsli`)与 C++ 里的内嵌源码不受扩展名规则影响。
改完记得重编:缓存键含源码与注解哈希,不会串用旧产物。

## 9. 材质函数(`assets/shaders/lib/*.slang`)

材质函数 = **可复用的纯函数**:一份图案/渐变/调色逻辑写一次,多个材质 include 后用。
示例库与真实用法:[`projects/default/assets/shaders/lib/pattern.slang`](../../projects/default/assets/shaders/lib/pattern.slang)
(`ApplyTint` / `CheckerPattern` / `CheckerBlend` / `RadialMask`);
示例材质 [`ShowcaseMaterial.slang`](../../projects/default/assets/shaders/examples/ShowcaseMaterial.slang)
include 了它,并在 `Evaluate()` 里用 `ApplyTint` → `CheckerBlend` 做棋盘细节。

**怎么创建**:编辑器里走内容浏览器 `New ▸ Material Shader…`,起始代码选
**Material function (library)** —— 目录会自动落到 `assets/shaders/lib`(不存在时创建),
文件名就是 `#include` 里用的名字。也可以直接在项目内容根下新建
`assets/shaders/lib/<名字>.slang`(建议按用途命名,如 `pattern.slang`、`noise.slang`)。
一个文件一个主题;库文件本身**不是材质资产** —— 它没有入口,不能单独被渲染、不单独烘,
只能在材质里 `#include`(在编辑器里打开它 = 库文件形态:不投递预览编译)。

**怎么写**:

- **纯函数**:输入 → 输出。不声明 `Evaluate`,不读写材质的 `input` / `surface`(它们属于材质),
  不引用材质参数名(那会让库与某个材质绑死,别的材质 include 进来会编译不过)。
- **不能有自己的贴图/采样器**:库文件里不写 `Texture2D` / `Sampler2D` 变量声明 ——
  贴图槽位由**材质**的 `//! param Texture2D` 注解决定(§4/§5)。需要采样时把
  `Sampler2D` 作为形参传进来:`float3 WeSampleTinted(Sampler2D map, float2 uv, float3 tint)`,
  材质侧调用 `WeSampleTinted(albedoMap, uv, tint.rgb)`。
- **参数带默认值**:末尾参数给默认值,调用点可以少写;Slang 不做隐式宽度转换,默认值类型要写对
  (`float2 center = float2(0.5, 0.5)`,不要指望 `0.5` 自动变成 `float2`)。
- **`doc` 与注解**:库里的说明用普通 `//` 注释(参数说明 `doc("…")` 是**材质注解**的字段,
  只属于 `//! param` 行;写在库文件里不会被任何东西读取)。
- **命名与版本**:共享库建议加 2–4 字母前缀,避免与材质自己的辅助函数、引擎包装符号重名(同一个
  翻译单元);破坏性改动**加新函数**(`CheckerPatternV2`)而不是改签名 —— 材质各自 include,
  改签名会一次打断所有调用点。

**怎么用**:材质里 `#include "lib/<名字>.slang"`,然后像普通函数一样调用。解析顺序是
**先材质自身目录、再项目 `assets/shaders` 根**,所以写 `lib/pattern.slang`(相对
`assets/shaders`)与 `my_helper.slang`(同目录)都能命中;库文件不要在 include 里用绝对路径
或 `../` 逃出内容根。

**改库 = 重烘**:表面着色器缓存键含**被包含文件的路径 + 内容哈希**(递归 `#include`),
所以改库一个字符,所有依赖它的材质会重新编译;发布包里的 baked 产物同样跟着更新。
新增材质函数**不需要**注册任何东西 —— 它只是被你自己的材质源码引用的普通 Slang 代码。

**边界:材质 vs 材质函数**

| | 材质(`assets/shaders/**/*.slang`) | 材质函数(`assets/shaders/lib/*.slang`) |
| --- | --- | --- |
| 入口 | 必须写 `Surface Evaluate(MaterialInputs input)` | 没有入口,只被 include |
| 参数 | `//! param …` 注解 = 面板行 / cbuffer 布局 / 贴图槽位 | 普通函数形参(+ 默认值),没有注解、不占槽位 |
| 资源 | 由注解生成 `Sampler2D` 绑定,`map.Sample(uv)` 采样 | 不能声明资源;需要就作为形参传入 |
| 管线 | 编译成双目标 SPIR-V、进缓存、进包 | 不单独编译/不单独进包,随引用它的材质一起烘 |
| 改动影响 | 只有该材质重烘 | 所有依赖它的材质重烘 |

最小对照示例(与 `lib/pattern.slang` 的写法一致):

```hlsl
// assets/shaders/lib/tint.slang —— 材质函数:纯数值,无绑定
float3 ApplyTint(float3 baseColor, float3 tint, float amount = 1.0)
{
    return lerp(baseColor, baseColor * tint, saturate(amount));
}

// assets/shaders/examples/MyMaterial.slang —— 材质:入口 + 注解 + 调用库
#include "lib/tint.slang"

//! param Color Tint = 0.85, 0.72, 0.55, 1 group("Surface") label("Tint") doc("表面基色。")

Surface Evaluate(MaterialInputs input)
{
    Surface surface = MakeDefaultSurface();
    surface.BaseColor = ApplyTint(surface.BaseColor, Tint.rgb, 0.5);
    return surface;
}
```

## 10. 边界与未验证

- 跨厂商未验证:GL_SPIRV + 组合采样器是驱动敏感区,目前只在 NVIDIA 610.62 上实测;换 A 卡/N 卡外驱动要重跑抓图。
- 材质表面着色器的 **OpenGL 实时预览**尚未接入(`MaterialSurfaceRuntime::Install` 目前只允许 Vulkan),
  归 M4-S4;GL 产物本身(`spirv-val --target-env opengl4.5` = 0、SPIR-V 1.0)已经就绪。
- `OriginUpperLeft`(Vulkan)与 GL lower-left 在 derivative/法线贴图上的差异还没有含法线贴图的门禁用例。
- BC5 法线重建目前只接在**包装层**(用材质着色器的材质,M4-TEX P2)。引擎标准着色器
  [`Renderer3D_Solid.slang`](../../Engine/assets/shaders/Renderer3D_Solid.slang) 的法线采样
  (`u_Flags.y` 分支)还没有 BC5 重建 —— 那条路径要用 `u_Flags.w`(当前空闲)或一个 `#define` 补同样的
  `z = sqrt(saturate(1 - x² - y²))`,接线点分别是该 shader 与 `Renderer3D.cpp` 的 `u_Flags` 打包处。
  未接线前,把 BC5 产物用在标准材质法线槽会得到 z≈-1 的退化法线(光照错误)。
