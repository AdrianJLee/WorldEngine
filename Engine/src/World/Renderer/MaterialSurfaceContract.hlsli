// WorldEngine M4-S1:材质表面函数契约(HLSL / C++ 共用同一份字段表)。
//
// 这个文件故意写成双语:
//   - Slang(slangc)编译生成的包装源码时,走 HLSL 分支,声明 MaterialInputs / Surface;
//   - C++ 编译单元包含它时,走 C++ 分支,拿到同一份字段元数据(名字/HLSL 类型/语义/默认值)。
// X-macro 列表是**唯一事实源**:结构体成员、默认表面函数、C++ 元数据都由它展开,
// 新增字段只改列表,不在别处手写成员。
//
// 口径:
//   - MaterialInputs 只放引擎当前**真的能给**的量(Renderer3D_Solid.hlsl 的顶点/像素阶段);
//     引擎没有相机位置 uniform、没有顶点色、没有 per-vertex 切线,所以这些字段不在这里;
//   - Surface.BaseColor / Surface.Emissive 是**线性 RGB**;显示空间编码由引擎包装层负责;
//   - Surface.Normal 是切线空间扰动,默认 (0,0,1) = 保留引擎采样到的法线(无法线贴图时 = 几何法线);
//   - 标量 Surface 字段语义范围 [0,1];包装层在参与光照前会 saturate。
#ifndef WORLD_MATERIAL_SURFACE_CONTRACT_HLSLI
#define WORLD_MATERIAL_SURFACE_CONTRACT_HLSLI

// 名称, HLSL 类型, 语义(C++ 侧字符串化;HLSL 侧忽略)
#define WE_MATERIAL_INPUT_FIELDS(X) \
    X(UV, float2, mesh UV0 from the vertex stream) \
    X(WorldPosition, float3, interpolated world-space position) \
    X(WorldNormal, float3, normalized world-space geometric normal) \
    X(WorldTangent, float3, world-space tangent built from screen-space derivatives) \
    X(WorldBitangent, float3, world-space bitangent = cross(WorldNormal, WorldTangent))

// 名称, HLSL 类型, 语义, 默认表达式(默认表达式只在引擎生成的 MakeDefaultSurface() 里使用)。
// 注意:默认表达式里**不能出现逗号** —— MSVC 的传统预处理器会在 X-macro 二层展开时
// 把未加保护的逗号重新拆成宏参数(C4002)。需要多参数构造时用模板里的无逗号辅助函数。
#define WE_SURFACE_FIELDS(X) \
    X(BaseColor, float3, linear RGB base color, WeLinearizeColor(u_BaseColor.rgb)) \
    X(Metallic, float, metallic in 0..1, saturate(u_MetallicRoughness.x)) \
    X(Roughness, float, roughness in 0..1, saturate(u_MetallicRoughness.y)) \
    X(Emissive, float3, linear RGB emissive, WeLinearizeColor(u_Emissive.rgb)) \
    X(Normal, float3, tangent-space normal perturbation, WeDefaultNormal()) \
    X(Opacity, float, opacity in 0..1, saturate(u_BaseColor.a)) \
    X(AmbientOcclusion, float, ambient occlusion in 0..1, 1.0f)

#ifdef __cplusplus
namespace World::MaterialSurfaceContract
{
    struct FieldInfo
    {
        const char* Name;
        const char* HlslType;
        const char* Semantic;
        const char* DefaultExpression; // MaterialInputs 字段为空字符串
    };

#define WE_FIELD_META3(name, hlslType, semantic) { #name, #hlslType, #semantic, "" },
#define WE_FIELD_META4(name, hlslType, semantic, defaultExpression) { #name, #hlslType, #semantic, #defaultExpression },

    inline constexpr FieldInfo MaterialInputFields[] = { WE_MATERIAL_INPUT_FIELDS(WE_FIELD_META3) };
    inline constexpr FieldInfo SurfaceFields[] = { WE_SURFACE_FIELDS(WE_FIELD_META4) };

#undef WE_FIELD_META3
#undef WE_FIELD_META4
}
#else
#define WE_FIELD_DECL2(name, hlslType, semantic) hlslType name;
#define WE_FIELD_DECL4(name, hlslType, semantic, defaultExpression) hlslType name;

struct MaterialInputs
{
    WE_MATERIAL_INPUT_FIELDS(WE_FIELD_DECL2)
};

struct Surface
{
    WE_SURFACE_FIELDS(WE_FIELD_DECL4)
};

#undef WE_FIELD_DECL2
#undef WE_FIELD_DECL4
#endif

#endif
