// ShowcaseMaterial — WorldEngine 材质着色器(Slang 源)特性总览
// ---------------------------------------------------------------------------
// 这是一个可直接被编辑器打开的**真实材质**:它同时展示
//   ① 表面函数契约(Evaluate / MaterialInputs / Surface)
//   ② 参数注解(//! param …)= 面板字段与默认值的**唯一事实源**
//   ③ 三条硬规则:严格类型 · 组合采样器 · 显式绑定(绑定由引擎生成,用户不用写)
//   ④ Slang 语言特性:泛型函数、interface + 实现、结构体构造
//   ⑤ **材质函数**:可复用的纯函数放 assets/shaders/lib/*.slang,`#include "lib/…"` 后直接调用
//
// 三条硬规则(与 docs/dev/shader-contract.md 一致):
//   - **严格类型**:`float4 → float3` 必须显式 `.xyz`;`float3 += float4` 不合法。
//   - **组合采样器**:贴图参数生成 `Sampler2D`,采样写 `map.Sample(uv)`(没有 sampler 参数)。
//   - **显式绑定**:资源声明由引擎按注解生成 `[[vk::binding(N, S)]]`,用户源里不写资源。
//
// 参数注解语法:`//! param <类型> <名字> = <默认值> [min,max] unit("x") group("G") label("L") doc("说明")`
// 可用类型:Float Vec2 Vec3 Vec4 Color Int Bool Texture2D
//
// **材质函数怎么写、放哪里**:纯函数放进 `assets/shaders/lib/<名字>.slang`(文件头写了规则),
// 材质侧 `#include "lib/<名字>.slang"` 后直接调用 —— 本文件 include 了 `lib/pattern.slang`,
// 在 Evaluate 里用 CheckerBlend/ApplyTint 做棋盘细节(见下面「材质函数」一段)。
// 细节与边界(材质 vs 材质函数)见 docs/dev/shader-contract.md「材质函数」一节;
// 库文件改了会让依赖它的材质重新烘焙(缓存键含被包含文件的哈希)。
// ---------------------------------------------------------------------------

// 材质函数库:纯函数(无绑定、无贴图、无入口);资源/参数一律由本材质传入。
#include "lib/pattern.slang"

//! param Color     tint            = 0.85, 0.72, 0.55, 1 group("Surface") label("Tint") doc("表面基色;与贴图相乘(线性空间)。")
//! param Float     roughness       = 0.35 [0.02, 1] group("Surface") label("Roughness") doc("粗糙度:0 = 镜面,1 = 全漫反射。")
//! param Float     metallic        = 0.10 [0, 1] group("Surface") label("Metallic") doc("金属度:0 = 绝缘体,1 = 金属(改变漫反射/高光配比)。")
//! param Vec3      sheen           = 0.10, 0.20, 0.30 group("Surface") label("Sheen Tint") doc("边缘偏色;也用作棋盘图案的图案色。")
//! param Int       shadingMode     = 0 [0, 2] group("Surface") label("Shading Mode (0/1/2)") doc("着色模式:0 常规 PBR,1 纯色调试,2 金属化。")
//! param Texture2D albedoMap       = "textures/Icon.png" group("Maps") label("Albedo") doc("基色贴图;空 = 引擎白色 1x1 兜底。")
//! param Texture2D normalMap       = "textures/flat_normal.png" group("Maps") label("Normal") doc("切线空间法线贴图;请指向真实贴图,否则光照会退化。")
//   ↑ 法线贴图的默认值必须指向**真实**的平面法线贴图:空路径/缺失会绑到引擎的白色 1×1,
//     于是 `texel*2-1` = (1,1,1) 退化法线 → 光照几乎为 0(表现为全黑)。
//! param Bool      useNormalMap    = true group("Maps") label("Use Normal Map") doc("关掉则忽略法线贴图,保留引擎采样到的法线。")
//! param Vec2      uvScale         = 1, 1 group("Maps") label("UV Scale") doc("UV 平铺系数(所有贴图与图案共用)。")
//! param Float     emissivePower   = 0.0 [0, 8] unit("x") group("Emissive") label("Emissive Power") doc("自发光强度倍数;0 = 不发光。")
//! param Color     emissiveTint    = 0.2, 0.6, 1.0, 1 group("Emissive") label("Emissive Tint") doc("自发光颜色 sRGB(写进 Surface 前会线性化)。")
//! param Float     opacity         = 1.0 [0, 1] group("Blend") label("Opacity") doc("不透明度;1 = 完全不透明。")
//! param Float     ambientOcclusion = 1.0 [0, 1] group("Blend") label("Ambient Occlusion") doc("环境光遮蔽;1 = 不遮蔽。")
// 材质函数参数:两个旋钮都进面板,库函数本身不带任何绑定(纯函数)。
//! param Float     patternScale    = 8.0 [1, 64] group("Pattern") label("Pattern Scale") doc("棋盘密度(uv 尺度);越大格越密。")
//! param Float     patternAmount   = 0.35 [0, 1] group("Pattern") label("Pattern Amount") doc("棋盘细节混入基色的比例;0 = 关掉图案。")
//! param Vec4      debugVector     = 0, 0, 0, 0 group("Advanced") label("Vec4 Demo")

// ---- Slang 特性:interface + 实现 + **泛型约束** -------------------------------
// 用户侧可以直接用 Slang 的抽象能力:同一份泛型函数接受任意"UV 变换"实现。
// (写法要点:泛型约束写成 `<T : 接口名>`;不要对未约束的 T 直接做 `T * float` —— Slang 不做隐式广播。)
interface IUVTransform
{
    float2 apply(float2 uv);
}

struct TiledUV : IUVTransform
{
    float2 scale;

    float2 apply(float2 uv)
    {
        return uv * scale;
    }
}

struct ScrolledUV : IUVTransform
{
    float2 offset;

    float2 apply(float2 uv)
    {
        return uv + offset;
    }
}

float2 ApplyUVTransform<T : IUVTransform>(T transform, float2 uv)
{
    return transform.apply(uv);
}

// ---- 表面函数:引擎唯一的入口 -------------------------------------------------
// MaterialInputs 由引擎填充(UV / WorldPosition / WorldNormal / WorldTangent / WorldBitangent);
// Surface 从 MakeDefaultSurface() 起步,只覆盖你关心的字段。
Surface Evaluate(MaterialInputs input)
{
    Surface surface = MakeDefaultSurface();

    // 结构体构造 + 接口实现 + 泛型调用(Slang 特性三连)
    TiledUV tiler = { uvScale };
    float2 uv = ApplyUVTransform(tiler, input.UV);

    // 组合采样器:没有 sampler 参数
    float3 albedo = albedoMap.Sample(uv).rgb;

    // 切线空间法线:用引擎给的世界切线基构造
    if (useNormalMap)
    {
        float3 tangentNormal = normalMap.Sample(uv).xyz * 2.0 - 1.0;
        surface.Normal = normalize(tangentNormal);   // 归一化,避免贴图内容退化时产生异常法线
    }

    // 三种着色模式 = 参数驱动分支
    if (shadingMode == 0)
    {
        // 常规:PBR 基色 × 参数色
        surface.BaseColor = albedo * tint.rgb;
        surface.Metallic = metallic;
        surface.Roughness = roughness;
    }
    else if (shadingMode == 1)
    {
        // 纯色调试:忽略贴图,只看几何+光照
        surface.BaseColor = tint.rgb;
        surface.Metallic = 0.0;
        surface.Roughness = 1.0;
    }
    else
    {
        // 金属化:基色去色调 + 高金属低粗糙
        float luma = dot(albedo, float3(0.2126, 0.7152, 0.0722));
        float3 grey = float3(luma, luma, luma);
        surface.BaseColor = albedo * 0.2 + grey * 0.8;   // 去色 80%(严格类型:两端都是 float3)
        surface.BaseColor = surface.BaseColor * tint.rgb;
        surface.Metallic = 0.9;
        surface.Roughness = 0.15;
    }

    // sheen 混入 15%(普通向量运算;未约束泛型不做隐式广播)
    surface.BaseColor = surface.BaseColor * 0.85 + sheen * 0.15;

    // ---- 材质函数(assets/shaders/lib/pattern.slang):棋盘细节 -------------------------
    // 纯函数只做数值运算:图案色与密度由**材质参数**传入,库函数自己不声明贴图/采样器/绑定。
    // 组合顺序 ApplyTint → CheckerBlend(库内 CheckerBlend 又调用 CheckerPattern)。
    float3 patternColor = ApplyTint(surface.BaseColor, sheen, 0.6);   // 图案色 = 基色往 sheen 偏 60%
    float3 patterned = CheckerBlend(surface.BaseColor, patternColor, uv, patternScale);
    surface.BaseColor = lerp(surface.BaseColor, patterned, patternAmount);

    // 自发光:颜色先线性化(引擎提供的辅助函数),再乘强度(单位 x)
    surface.Emissive = WeLinearizeColor(emissiveTint.rgb) * emissivePower;

    // 混合与 AO
    surface.Opacity = opacity;
    surface.AmbientOcclusion = ambientOcclusion;

    return surface;
}

// 说明(可删):debugVector 当前不参与着色,用于演示"声明但未用"时引擎给出的可读警告;
// 想让它生效时,把它接进某个字段(例如 float ao = ambientOcclusion * debugVector.w;)。
// 材质函数:库文件在 assets/shaders/lib/pattern.slang(ApplyTint / CheckerPattern / CheckerBlend /
// RadialMask);想换图案时改 patternScale/patternAmount,想加新函数时在库里加一个**新名字**。
