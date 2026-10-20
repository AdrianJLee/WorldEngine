// WorldEngine 3D Terrain Shader (Slang)
//  - Set 0: Global Camera / Light / Shadow (matches Renderer::GetGlobalDescriptorSetLayout)
//  - Set 1: Per-Terrain Object Uniforms (binding 1: TerrainUniforms)
//  - Set 2: 4-Layer Surface Textures (bindings 4..7: Layer 0..3 Albedo)

struct VS_INPUT
{
    [[vk::location(0)]] float3 a_Position : POSITION;
    [[vk::location(1)]] float3 a_Normal : NORMAL;
    [[vk::location(2)]] float2 a_TexCoord : TEXCOORD0;
    [[vk::location(3)]] float4 a_SplatWeights : SPLATWEIGHTS;
    [[vk::location(4)]] float a_Hole : HOLE;
};

struct VS_OUTPUT
{
    float4 Position : SV_Position;
    [[vk::location(0)]] float3 v_Normal : NORMAL;
    [[vk::location(1)]] float3 v_WorldPosition : POSITION1;
    [[vk::location(2)]] float2 v_TexCoord : TEXCOORD0;
    [[vk::location(3)]] float4 v_SplatWeights : SPLATWEIGHTS;
    [[vk::location(4)]] nointerpolation float v_Hole : HOLE;
    [[vk::location(5)]] nointerpolation float v_EntityId : ENTITYID;
};

struct PS_OUTPUT
{
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target1;
};

// ==========================================
// Set 0: 全局相机与灯光系统 (与 Renderer3D 一致)
// ==========================================
[[vk::binding(0, 0)]] cbuffer CameraUniforms
{
    float4x4 u_ViewProjection;
};

struct GpuLight
{
    float4 PositionType;      // xyz = 位置(点光) / w = 0 点光 | 1 方向光
    float4 ColorIntensity;    // rgb = 线性色, a = 强度
    float4 DirectionRange;    // 方向光:xyz = 传播方向(已归一化);点光:x = 范围
};

[[vk::binding(2, 0)]] cbuffer LightUniforms
{
    float4x4 u_ShadowViewProjection;
    // x = 启用阴影, y = 深度 bias, z = 贴图边长, w = PCF 半径(纹素)
    float4 u_ShadowParams;
    // rgb = 环境光线性色, a = 强度
    float4 u_Ambient;
    // x = 方向光数, y = 点光数, z = 深度约定(0 = Vulkan [0,1], 1 = OpenGL [-1,1])
    uint4 u_LightCounts;
    GpuLight u_Lights[8];
};

[[vk::binding(3, 0)]] Sampler2D u_ShadowMap;

// ==========================================
// Set 1: 地形对象级参数 (UBO binding 1, 避免与 Set 0 单元冲突)
// ==========================================
[[vk::binding(1, 1)]] cbuffer TerrainUniforms
{
    float4x4 u_Model;
    float4 u_LayerTiling01;         // xy = Layer 0 tiling, zw = Layer 1 tiling
    float4 u_LayerTiling23;         // xy = Layer 2 tiling, zw = Layer 3 tiling
    float4 u_LayerRoughnessMetallic;// x,y,z,w = Roughness for layers 0..3
    float4 u_SlopeParams;           // x = slopeThreshold (0.707), y = slopeBlendRange (0.15), z = rockLayerIndex (2.0), w = holeCutoutEnabled (1.0)
    float4 u_TerrainParams;         // x = receiveShadow, y = entityId, zw = reserved
};

// ==========================================
// Set 2: 4 层材质纹理 (bindings 4..7, 避开 Set 0 的 3 号阴影纹理单元)
// ==========================================
[[vk::binding(4, 2)]] Sampler2D u_Layer0Texture; // Layer 0: 草地 (Grass)
[[vk::binding(5, 2)]] Sampler2D u_Layer1Texture; // Layer 1: 泥土 (Dirt)
[[vk::binding(6, 2)]] Sampler2D u_Layer2Texture; // Layer 2: 岩石 (Rock)
[[vk::binding(7, 2)]] Sampler2D u_Layer3Texture; // Layer 3: 沙地 (Sand)

// 阴影采样 PCF (3x3 盒式滤波)
float SampleDirectionalShadow(float3 worldPosition, float3 normal, float3 toLightDirection)
{
    const float4 shadowPosition = mul(u_ShadowViewProjection, float4(worldPosition, 1.0f));
    if (shadowPosition.w <= 0.0f)
        return 1.0f;
    const float3 projected = shadowPosition.xyz / shadowPosition.w;
    const float depth = (u_LightCounts.z > 0u) ? (projected.z * 0.5f + 0.5f) : projected.z;
    const float2 uv = projected.xy * 0.5f + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        return 1.0f;

    const float cosTheta = saturate(dot(normal, toLightDirection));
    const float bias = max(u_ShadowParams.y * (1.0f - cosTheta), u_ShadowParams.y * 0.25f);
    const float2 texel = u_ShadowParams.w / max(u_ShadowParams.z, 1.0f);
    float visible = 0.0f;
    [unroll] for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        [unroll] for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            const float sampledDepth = u_ShadowMap.Sample(
                uv + float2(offsetX, offsetY) * texel).r;
            visible += (depth - bias <= sampledDepth) ? 1.0f : 0.0f;
        }
    }
    return visible / 9.0f;
}

// ==========================================
// 主通道 (Main Forward Pass)
// ==========================================

VS_OUTPUT VSMain(VS_INPUT input)
{
    VS_OUTPUT output;
    float4 worldPos = mul(u_Model, float4(input.a_Position, 1.0f));
    output.v_WorldPosition = worldPos.xyz;
    output.Position = mul(u_ViewProjection, worldPos);

    // 变换法线到世界空间
    output.v_Normal = normalize(mul((float3x3)u_Model, input.a_Normal));
    output.v_TexCoord = input.a_TexCoord;
    output.v_SplatWeights = input.a_SplatWeights;
    output.v_Hole = input.a_Hole;
    output.v_EntityId = u_TerrainParams.y;
    return output;
}

PS_OUTPUT PSMain(VS_OUTPUT input)
{
    PS_OUTPUT output;

    // 1. 地洞镂空剔除
    if (u_SlopeParams.w > 0.5f && input.v_Hole > 0.5f)
        discard;

    // 2. 采样 4 层材质纹理
    float2 uv0 = input.v_TexCoord * u_LayerTiling01.xy;
    float2 uv1 = input.v_TexCoord * u_LayerTiling01.zw;
    float2 uv2 = input.v_TexCoord * u_LayerTiling23.xy;
    float2 uv3 = input.v_TexCoord * u_LayerTiling23.zw;

    float4 col0 = u_Layer0Texture.Sample(uv0);
    float4 col1 = u_Layer1Texture.Sample(uv1);
    float4 col2 = u_Layer2Texture.Sample(uv2);
    float4 col3 = u_Layer3Texture.Sample(uv3);

    float3 c0 = pow(saturate(col0.rgb), 2.2f);
    float3 c1 = pow(saturate(col1.rgb), 2.2f);
    float3 c2 = pow(saturate(col2.rgb), 2.2f);
    float3 c3 = pow(saturate(col3.rgb), 2.2f);

    // 3. 初始 Splat 权重归一化
    float4 weights = saturate(input.v_SplatWeights);
    float totalWeight = weights.x + weights.y + weights.z + weights.w;
    if (totalWeight > 0.0001f)
        weights /= totalWeight;
    else
        weights = float4(1.0f, 0.0f, 0.0f, 0.0f);

    // 4. 自适应坡度混合 (Slope Blend)
    // 当法线与垂直朝上夹角过大（dot(normal, vec3(0,1,0)) < 0.707，即斜率 > 45 度）时，自动向岩石材质层过渡
    float3 worldNormal = normalize(input.v_Normal);
    float upDot = saturate(dot(worldNormal, float3(0.0f, 1.0f, 0.0f)));
    float slopeThreshold = u_SlopeParams.x; // 默认 0.707f
    float slopeBlendRange = max(u_SlopeParams.y, 0.001f); // 默认 0.15f
    
    float rockFactor = saturate((slopeThreshold - upDot) / slopeBlendRange);
    
    // 岩石对应 Layer 2 (weights.z)
    weights.x *= (1.0f - rockFactor);
    weights.y *= (1.0f - rockFactor);
    weights.w *= (1.0f - rockFactor);
    weights.z = weights.z * (1.0f - rockFactor) + rockFactor;

    float finalTotal = weights.x + weights.y + weights.z + weights.w;
    if (finalTotal > 0.0001f)
        weights /= finalTotal;

    // 混合 Albedo 与 Roughness
    float3 albedo = c0 * weights.x + c1 * weights.y + c2 * weights.z + c3 * weights.w;
    float roughness = dot(u_LayerRoughnessMetallic, weights);
    float metallic = 0.0f;

    // 5. 光照与阴影渲染 (PBR Lambert + 粗糙度 Specular)
    const float3 viewDirection = float3(0.0f, 0.0f, 1.0f);
    float3 litColor = albedo * u_Ambient.rgb * u_Ambient.a;

    const uint lightCount = u_LightCounts.x + u_LightCounts.y;
    for (uint lightIndex = 0; lightIndex < lightCount; ++lightIndex)
    {
        const GpuLight light = u_Lights[lightIndex];
        const bool isDirectional = light.PositionType.w > 0.5f;
        float3 toLight;
        float attenuation = 1.0f;
        if (isDirectional)
        {
            toLight = -normalize(light.DirectionRange.xyz);
        }
        else
        {
            const float3 offset = light.PositionType.xyz - input.v_WorldPosition;
            const float distanceToLight = length(offset);
            toLight = distanceToLight > 1e-5f ? offset / distanceToLight : float3(0.0f, 0.0f, 0.0f);
            const float falloff = saturate(1.0f - distanceToLight / max(light.DirectionRange.x, 1e-4f));
            attenuation = falloff * falloff;
        }

        const float lambert = saturate(dot(worldNormal, toLight));
        float visibility = 1.0f;
        if (isDirectional && u_ShadowParams.x > 0.5f && u_TerrainParams.x > 0.5f)
        {
            visibility = SampleDirectionalShadow(input.v_WorldPosition, worldNormal, toLight);
        }

        const float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.a * attenuation * visibility;
        const float diffuse = lambert * lerp(1.0f, 0.35f, saturate(metallic));
        const float3 halfVector = normalize(toLight + viewDirection + 1e-5f);
        const float specularPower = lerp(8.0f, 128.0f, saturate(1.0f - roughness));
        const float specular = lambert > 0.0f
            ? pow(saturate(dot(worldNormal, halfVector)), specularPower) * lerp(0.04f, 1.0f, saturate(metallic))
            : 0.0f;
        litColor += albedo * radiance * diffuse + radiance * specular;
    }

    // 线性到显示空间转换
    output.Color = float4(pow(saturate(litColor), 1.0f / 2.2f), 1.0f);
    output.EntityID = (int)round(input.v_EntityId);
    return output;
}

// ==========================================
// 阴影通道 (Shadow Pass)
// ==========================================

struct VS_SHADOW_OUTPUT
{
    float4 Position : SV_Position;
    [[vk::location(0)]] nointerpolation float v_Hole : HOLE;
};

VS_SHADOW_OUTPUT VSShadowMain(VS_INPUT input)
{
    VS_SHADOW_OUTPUT output;
    float4 worldPos = mul(u_Model, float4(input.a_Position, 1.0f));
    output.Position = mul(u_ShadowViewProjection, worldPos);
    output.v_Hole = input.a_Hole;
    return output;
}

void PSShadowMain(VS_SHADOW_OUTPUT input)
{
    if (u_SlopeParams.w > 0.5f && input.v_Hole > 0.5f)
        discard;
}
