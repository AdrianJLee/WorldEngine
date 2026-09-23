// ShowcaseMaterial — WorldEngine 材质着色器(Slang 源)特性总览
// ---------------------------------------------------------------------------
// 这是一个可直接被编辑器打开的**真实材质**:它同时展示
//   ① 表面函数契约(Evaluate / MaterialInputs / Surface)
//   ② 参数注解(//! param …)= 面板字段与默认值的**唯一事实源**
//   ③ 三条硬规则:严格类型 · 组合采样器 · 显式绑定(绑定由引擎生成,用户不用写)
//   ④ Slang 语言特性:泛型函数、interface + 实现、结构体构造
//
// 三条硬规则(与 docs/dev/shader-contract.md 一致):
//   - **严格类型**:`float4 → float3` 必须显式 `.xyz`;`float3 += float4` 不合法。
//   - **组合采样器**:贴图参数生成 `Sampler2D`,采样写 `map.Sample(uv)`(没有 sampler 参数)。
//   - **显式绑定**:资源声明由引擎按注解生成 `[[vk::binding(N, S)]]`,用户源里不写资源。
//
// 参数注解语法:`//! param <类型> <名字> = <默认值> [min,max] unit("x") group("G") label("L")`
// 可用类型:Float Vec2 Vec3 Vec4 Color Int Bool Texture2D
// ---------------------------------------------------------------------------

//! param Color     tint            = 0.85, 0.72, 0.55, 1 group("Surface") label("Tint")
//! param Float     roughness       = 0.35 [0.02, 1] group("Surface") label("Roughness")
//! param Float     metallic        = 0.10 [0, 1] group("Surface") label("Metallic")
//! param Vec3      sheen           = 0.10, 0.20, 0.30 group("Surface") label("Sheen Tint")
//! param Int       shadingMode     = 0 [0, 2] group("Surface") label("Shading Mode (0/1/2)")
//! param Texture2D albedoMap       = "textures/Icon.png" group("Maps") label("Albedo")
//! param Texture2D normalMap       = "" group("Maps") label("Normal")
//! param Bool      useNormalMap    = true group("Maps") label("Use Normal Map")
//! param Vec2      uvScale         = 1, 1 group("Maps") label("UV Scale")
//! param Float     emissivePower   = 0.0 [0, 8] unit("x") group("Emissive") label("Emissive Power")
//! param Color     emissiveTint    = 0.2, 0.6, 1.0, 1 group("Emissive") label("Emissive Tint")
//! param Float     opacity         = 1.0 [0, 1] group("Blend") label("Opacity")
//! param Float     ambientOcclusion = 1.0 [0, 1] group("Blend") label("Ambient Occlusion")
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
        surface.Normal = tangentNormal;
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

    // 自发光:颜色先线性化(引擎提供的辅助函数),再乘强度(单位 x)
    surface.Emissive = WeLinearizeColor(emissiveTint.rgb) * emissivePower;

    // 混合与 AO
    surface.Opacity = opacity;
    surface.AmbientOcclusion = ambientOcclusion;

    return surface;
}

// 说明(可删):debugVector 当前不参与着色,用于演示"声明但未用"时引擎给出的可读警告;
// 想让它生效时,把它接进某个字段(例如 float ao = ambientOcclusion * debugVector.w;)。
