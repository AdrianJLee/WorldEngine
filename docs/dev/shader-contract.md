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
> **两条硬性写法**(踩过才写在这里):① **注解行必须独占一行** —— `//! param …` 行尾不能再跟 `// 注释`;
> ② `[min,max]` **只适用于 Float/Int**(写在 Vec2/Vec3/Color 上会解析失败)。

## 1. 一个材质着色器长什么样

新建 `.slang` 时照这个骨架写(改注解与 `Evaluate()` 里的赋值即可;下面代码块的语法分类是 HLSL 家族,
Slang 不需要任何语法改写):

```hlsl
//! param Color Tint = 1, 1, 1, 1 group("Appearance") label("Tint")
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

## 6. `//! param` 注解语法

```
//! param <type> <name> = <default> [min,max] unit("…") group("…") label("…")
```

- `<type>`:`Float` / `Vec2` / `Vec3` / `Vec4` / `Color` / `Int` / `Bool` / `Texture2D`(大小写敏感)。
- `<default>` **必填**:数值是逗号分隔字面量(`0.25`、`1, 0.5, 0.25, 1`);`Bool` 是 `true/false`;
  `Texture2D` 是内容根相对路径(`""` = 无贴图)。
- `[min,max]` 只对 `Float`/`Int` 有效(默认 `[0,1]`),默认值必须落在区间内。
- `unit/group/label` 是双引号字符串,可空;每条注解里每个字段最多出现一次。
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

## 9. 边界与未验证

- 跨厂商未验证:GL_SPIRV + 组合采样器是驱动敏感区,目前只在 NVIDIA 610.62 上实测;换 A 卡/N 卡外驱动要重跑抓图。
- 材质表面着色器的 **OpenGL 实时预览**尚未接入(`MaterialSurfaceRuntime::Install` 目前只允许 Vulkan),
  归 M4-S4;GL 产物本身(`spirv-val --target-env opengl4.5` = 0、SPIR-V 1.0)已经就绪。
- `OriginUpperLeft`(Vulkan)与 GL lower-left 在 derivative/法线贴图上的差异还没有含法线贴图的门禁用例。
