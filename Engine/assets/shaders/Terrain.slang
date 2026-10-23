// ==========================================
// WorldEngine Next-Gen Commercial Landscape PBR Shader
// ==========================================
//  - Set 0: Global Camera / Light / Shadow (matches Renderer::GetGlobalDescriptorSetLayout)
//  - Set 1: Per-Terrain Object Uniforms (binding 1: TerrainUniforms 160B std140)
//  - Set 2: 4-Layer PBR Surface Textures (bindings 4..7: Albedo, bindings 8..11: Normal)

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
// Set 0: 全局相机与光影系统
// ==========================================
[[vk::binding(0, 0)]] cbuffer CameraUniforms
{
    float4x4 u_ViewProjection;
};

struct GpuLight
{
    float4 PositionType;      // xyz = 位置(点/聚光) / w = 0 方向 | 1 点光源 | 2 聚光灯
    float4 ColorIntensity;    // rgb = 光源色, a = 强度
    float4 DirectionRange;    // 方向光:xyz = 照射方向;点光:x = 范围;聚光灯:xyz = 照射方向, w = 范围
    float4 SpotAngles;        // 聚光灯角度: x = cos(InnerAngle), y = cos(OuterAngle), zw = 保留
};

[[vk::binding(2, 0)]] cbuffer LightUniforms
{
    float4x4 u_ShadowViewProjections[4];
    float4 u_CascadeSplits;
    float4 u_ShadowParams;    // x = 级联阴影, y = 深度 bias, z = 贴图边长, w = PCF 半径
    float4 u_Ambient;         // rgb = 环境光颜色, a = 强度
    uint4 u_LightCounts;      // x = 方向光, y = 点光, z = 深度约束, w = 聚光灯数
    GpuLight u_Lights[8];
};

[[vk::binding(3, 0)]] Sampler2D u_ShadowMap;

// ==========================================
// Set 1: 地貌对象级参数 (160B std140)
// ==========================================
[[vk::binding(1, 1)]] cbuffer TerrainUniforms
{
    float4x4 u_Model;
    float4 u_LayerTiling01;         // xy = Layer 0 tiling, zw = Layer 1 tiling
    float4 u_LayerTiling23;         // xy = Layer 2 tiling, zw = Layer 3 tiling
    float4 u_LayerRoughnessMetallic;// x,y,z,w = Roughness for layers 0..3
    float4 u_SlopeParams;           // x = slopeThreshold (0.707), y = slopeBlendRange (0.15), z = rockLayerIndex (2.0), w = holeCutoutEnabled (1.0)
    float4 u_TerrainParams;         // x = receiveShadow, y = entityId, zw = reserved
    float4 u_CameraPosition;        // xyz = camera world position, w = 1.0
};

// ==========================================
// Set 2: 4 层工业级 PBR 地表材质 (Albedo: 4..7, Normal: 8..11)
// ==========================================
[[vk::binding(4, 2)]] Sampler2D u_Layer0Texture; // Layer 0: 草地 (Grass)
[[vk::binding(5, 2)]] Sampler2D u_Layer1Texture; // Layer 1: 泥土 (Dirt)
[[vk::binding(6, 2)]] Sampler2D u_Layer2Texture; // Layer 2: 岩石 (Rock)
[[vk::binding(7, 2)]] Sampler2D u_Layer3Texture; // Layer 3: 沙地 (Sand)

[[vk::binding(8, 2)]] Sampler2D u_Layer0Normal;  // Layer 0: 草地法线
[[vk::binding(9, 2)]] Sampler2D u_Layer1Normal;  // Layer 1: 泥土法线
[[vk::binding(10, 2)]] Sampler2D u_Layer2Normal; // Layer 2: 岩石法线
[[vk::binding(11, 2)]] Sampler2D u_Layer3Normal; // Layer 3: 沙地法线

// 法线解包工具
float3 UnpackNormal(float4 rawNormal)
{
    float3 n = rawNormal.xyz * 2.0f - 1.0f;
    n.z = sqrt(max(0.0f, 1.0f - dot(n.xy, n.xy)));
    return normalize(n);
}

// 阴影采样 PCF (4 级 CSM 2×2 Atlas 采样与 3x3 盒式滤波)
float SampleDirectionalShadow(float3 worldPosition, float3 normal, float3 toLightDirection, out int outCascadeIndex)
{
    outCascadeIndex = -1;
    float4 shadowPosition = float4(0, 0, 0, 1);
    float3 projected = float3(0, 0, 0);
    float2 localUV = float2(-1, -1);
    int matchedCascade = -1;

    [unroll] for (int c = 0; c < 4; ++c)
    {
        shadowPosition = mul(u_ShadowViewProjections[c], float4(worldPosition, 1.0f));
        projected = shadowPosition.xyz / max(shadowPosition.w, 1e-5f);
        localUV = projected.xy * 0.5f + 0.5f;
        if (localUV.x >= 0.0f && localUV.x <= 1.0f && localUV.y >= 0.0f && localUV.y <= 1.0f
            && (((u_LightCounts.z & 1u) > 0u) ? (projected.z * 0.5f + 0.5f) : projected.z) >= 0.0f && (((u_LightCounts.z & 1u) > 0u) ? (projected.z * 0.5f + 0.5f) : projected.z) <= 1.0f)
        {
            matchedCascade = c;
            break;
        }
    }

    if (matchedCascade < 0)
        return 1.0f; // 超出所有级联或视锥外，完全不受阴影遮挡

    outCascadeIndex = matchedCascade;

    // 映射至 2x2 Atlas 象限
    const float2 quadOffset = float2(float(matchedCascade % 2) * 0.5f, float(matchedCascade / 2) * 0.5f);
    const float2 atlasUV = quadOffset + localUV * 0.5f;

    const float depth = (u_LightCounts.z & 1u) > 0u ? (projected.z * 0.5f + 0.5f) : projected.z;
    const float cosTheta = saturate(dot(normal, toLightDirection));
    const float biasBase = max(u_ShadowParams.y, 0.002f);
    const float bias = max(biasBase * (1.0f - cosTheta), biasBase * 0.25f);
    const float2 texel = (u_ShadowParams.w / max(u_ShadowParams.z, 1.0f)) * 0.5f;

    float visible = 0.0f;
    [unroll] for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        [unroll] for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            float2 sampleUV = atlasUV + float2(offsetX, offsetY) * texel;
            sampleUV = clamp(sampleUV, quadOffset + texel * 0.5f, quadOffset + float2(0.5f, 0.5f) - texel * 0.5f);
            const float sampledDepth = u_ShadowMap.Sample(sampleUV).r;
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

    // 1. 地貌穿洞裁切
    if (u_SlopeParams.w > 0.5f && input.v_Hole > 0.5f)
        discard;

    // 2. 世界坐标连续微观扰动 (消除网格顶点插值造成的像素方格与色块边缘)
    float2 wp = input.v_WorldPosition.xz;
    float organicWarp1 = sin(wp.x * 0.35f + wp.y * 0.22f) * cos(wp.y * 0.31f - wp.x * 0.18f);
    float organicWarp2 = cos(wp.x * 0.85f - wp.y * 0.65f) * sin(wp.y * 0.75f + wp.x * 0.45f);
    float organicJitter = (organicWarp1 * 0.6f + organicWarp2 * 0.4f) * 0.08f;

    // 3. 采样 4 层 PBR 材质与双频抗平铺
    float2 uv0 = input.v_TexCoord * u_LayerTiling01.xy;
    float2 uv1 = input.v_TexCoord * u_LayerTiling01.zw;
    float2 uv2 = input.v_TexCoord * u_LayerTiling23.xy;
    float2 uv3 = input.v_TexCoord * u_LayerTiling23.zw;

    // 草地双频抗平铺 (Dual-Frequency Anti-Tiling 消除瓷砖重复感)
    float4 col0 = u_Layer0Texture.Sample(uv0);
    float4 macro0 = u_Layer0Texture.Sample(uv0 * 0.23f + float2(0.41f, 0.63f));
    float3 c0 = lerp(pow(saturate(col0.rgb), 2.2f), pow(saturate(macro0.rgb), 2.2f), 0.30f);

    float3 c1 = pow(saturate(u_Layer1Texture.Sample(uv1).rgb), 2.2f);
    float3 c2 = pow(saturate(u_Layer2Texture.Sample(uv2).rgb), 2.2f);

    // 沙地双频抗平铺 (自然流沙微波)
    float4 col3 = u_Layer3Texture.Sample(uv3);
    float4 macro3 = u_Layer3Texture.Sample(uv3 * 0.19f + float2(0.28f, 0.74f));
    float3 c3 = lerp(pow(saturate(col3.rgb), 2.2f), pow(saturate(macro3.rgb), 2.2f), 0.25f);

    float3 n0 = UnpackNormal(u_Layer0Normal.Sample(uv0));
    float3 n1 = UnpackNormal(u_Layer1Normal.Sample(uv1));
    float3 n2 = UnpackNormal(u_Layer2Normal.Sample(uv2));
    float3 n3 = UnpackNormal(u_Layer3Normal.Sample(uv3));

    // 4. 连续柔和高度融合 (Smooth Height Blend 消除生硬切断)
    float4 rawWeights = saturate(input.v_SplatWeights);
    // 叠加微观有机扰动
    rawWeights.x = saturate(rawWeights.x + organicJitter);
    rawWeights.y = saturate(rawWeights.y - organicJitter * 0.5f);
    float sumRaw = rawWeights.x + rawWeights.y + rawWeights.z + rawWeights.w;
    if (sumRaw > 0.0001f)
        rawWeights /= sumRaw;
    else
        rawWeights = float4(1.0f, 0.0f, 0.0f, 0.0f);

    // 提取微观高度
    float h0 = dot(c0, float3(0.299f, 0.587f, 0.114f));
    float h1 = dot(c1, float3(0.299f, 0.587f, 0.114f));
    float h2 = dot(c2, float3(0.299f, 0.587f, 0.114f));
    float h3 = dot(c3, float3(0.299f, 0.587f, 0.114f));

    // 采用平滑幂次加权 (Soft Height Blending 代替生硬的 Hard Threshold，彻底消除色块方格感)
    float4 heightBlend = rawWeights * (float4(h0, h1, h2, h3) * 0.40f + 0.60f);
    heightBlend = pow(heightBlend, 2.5f);
    float sumBlend = heightBlend.x + heightBlend.y + heightBlend.z + heightBlend.w;
    float4 weights = (sumBlend > 0.0001f) ? (heightBlend / sumBlend) : rawWeights;

    // 5. 陡峭悬崖三平面无拉伸投影 (Triplanar Cliff Mapping)
    float3 geomNormal = normalize(input.v_Normal);
    float upDot = saturate(dot(geomNormal, float3(0.0f, 1.0f, 0.0f)));
    float slopeThreshold = u_SlopeParams.x; // 默认 0.707f (45度)
    float slopeBlendRange = max(u_SlopeParams.y, 0.001f);
    float rockFactor = saturate((slopeThreshold - upDot) / slopeBlendRange);

    if (rockFactor > 0.01f)
    {
        float3 triWeight = pow(abs(geomNormal), 5.0f);
        triWeight /= max(triWeight.x + triWeight.y + triWeight.z, 1e-4f);

        float2 uvX = input.v_WorldPosition.zy * 0.07f;
        float2 uvY = input.v_WorldPosition.xz * 0.07f;
        float2 uvZ = input.v_WorldPosition.xy * 0.07f;

        float3 rockColX = pow(saturate(u_Layer2Texture.Sample(uvX).rgb), 2.2f);
        float3 rockColY = pow(saturate(u_Layer2Texture.Sample(uvY).rgb), 2.2f);
        float3 rockColZ = pow(saturate(u_Layer2Texture.Sample(uvZ).rgb), 2.2f);
        float3 rockTriAlbedo = rockColX * triWeight.x + rockColY * triWeight.y + rockColZ * triWeight.z;

        float3 rockNormX = UnpackNormal(u_Layer2Normal.Sample(uvX));
        float3 rockNormY = UnpackNormal(u_Layer2Normal.Sample(uvY));
        float3 rockNormZ = UnpackNormal(u_Layer2Normal.Sample(uvZ));
        float3 rockTriNormal = normalize(rockNormX * triWeight.x + rockNormY * triWeight.y + rockNormZ * triWeight.z);

        c2 = lerp(c2, rockTriAlbedo, rockFactor);
        n2 = lerp(n2, rockTriNormal, rockFactor);

        weights.x *= (1.0f - rockFactor);
        weights.y *= (1.0f - rockFactor);
        weights.w *= (1.0f - rockFactor);
        weights.z = weights.z * (1.0f - rockFactor) + rockFactor;

        float finalSum = weights.x + weights.y + weights.z + weights.w;
        if (finalSum > 1e-4f)
            weights /= finalSum;
    }

    // 6. 最终地表 Albedo、法线解包与真实粗糙度
    float3 albedo = c0 * weights.x + c1 * weights.y + c2 * weights.z + c3 * weights.w;

    // 宏观大地色调微扰 (Macro Color Variation 消除远山死板单调)
    float macroLuma = 1.0f + (sin(input.v_WorldPosition.x * 0.012f) * cos(input.v_WorldPosition.z * 0.012f)) * 0.08f;
    albedo *= macroLuma;

    float roughness = dot(u_LayerRoughnessMetallic, weights);
    float metallic = 0.0f;

    // 规则网格几何切线空间 (TBN)
    float3 tangent = normalize(float3(1.0f, 0.0f, 0.0f) - geomNormal * geomNormal.x);
    if (dot(tangent, tangent) < 1e-4f)
        tangent = float3(1.0f, 0.0f, 0.0f);
    float3 bitangent = normalize(cross(geomNormal, tangent));
    float3x3 TBN = float3x3(tangent, bitangent, geomNormal);

    float3 blendedTangentNormal = normalize(n0 * weights.x + n1 * weights.y + n2 * weights.z + n3 * weights.w);
    float3 worldNormal = normalize(mul(blendedTangentNormal, TBN));

    // 7. 物理视线与全动态 PBR 光照
    float3 viewDirection = normalize(u_CameraPosition.xyz - input.v_WorldPosition);
    if (dot(viewDirection, viewDirection) < 1e-4f)
        viewDirection = float3(0.0f, 1.0f, 0.0f);

    // 次世代半球天空天光 (天顶蔚蓝柔光 + 地表散射反光)
    float3 skyLight = u_Ambient.rgb * (worldNormal.y * 0.35f + 0.65f) * u_Ambient.a;
    float3 litColor = albedo * skyLight;

    int cascadeIndex = -1;
    const uint lightCount = u_LightCounts.x + u_LightCounts.y + u_LightCounts.w;
    for (uint lightIndex = 0; lightIndex < lightCount && lightIndex < 8; ++lightIndex)
    {
        const GpuLight light = u_Lights[lightIndex];
        const float lightType = light.PositionType.w;
        float3 toLight;
        float attenuation = 1.0f;
        if (lightType > 0.5f && lightType < 1.5f)
        {
            toLight = -normalize(light.DirectionRange.xyz);
        }
        else if (lightType > 1.5f)
        {
            const float3 offset = light.PositionType.xyz - input.v_WorldPosition;
            const float distanceToLight = length(offset);
            toLight = distanceToLight > 1e-5f ? offset / distanceToLight : float3(0.0f, 0.0f, 0.0f);
            const float range = max(light.DirectionRange.w, 1e-4f);
            const float falloff = saturate(1.0f - distanceToLight / range);
            attenuation = falloff * falloff;

            const float3 spotDir = normalize(light.DirectionRange.xyz);
            const float cosAngle = dot(spotDir, -toLight);
            const float cosInner = light.SpotAngles.x;
            const float cosOuter = light.SpotAngles.y;
            const float spotFalloff = saturate((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4f));
            attenuation *= spotFalloff * spotFalloff;
        }
        else
        {
            const float3 offset = light.PositionType.xyz - input.v_WorldPosition;
            const float distanceToLight = length(offset);
            toLight = distanceToLight > 1e-5f ? offset / distanceToLight : float3(0.0f, 0.0f, 0.0f);
            const float falloff = saturate(1.0f - distanceToLight / max(light.DirectionRange.x, 1e-4f));
            attenuation = falloff * falloff;
        }

        const float NdotL = max(dot(worldNormal, toLight), 0.0f);
        float visibility = 1.0f;
        if (lightType > 0.5f && lightType < 1.5f && ((u_ShadowParams.x > 0.5f && u_TerrainParams.x > 0.5f) || (u_LightCounts.z & 0x100u) != 0u))
        {
            int cIndex = -1;
            const float shadowVis = SampleDirectionalShadow(input.v_WorldPosition, worldNormal, toLight, cIndex);
            if (u_ShadowParams.x > 0.5f && u_TerrainParams.x > 0.5f)
                visibility = shadowVis;
            if (cIndex >= 0)
                cascadeIndex = cIndex;
        }

        const float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.a * attenuation * visibility;
        
        // PBR 漫反射 (Lambert)
        const float3 diffuse = albedo * (NdotL * (1.0f - metallic));

        // PBR 高光 (GGX 近似)
        float3 H = normalize(toLight + viewDirection);
        float NdotH = max(dot(worldNormal, H), 0.0f);
        float VdotH = max(dot(viewDirection, H), 0.0f);
        float NdotV = max(dot(worldNormal, viewDirection), 0.0f);

        float alpha = max(roughness * roughness, 0.001f);
        float alpha2 = alpha * alpha;
        float denom = (NdotH * NdotH * (alpha2 - 1.0f) + 1.0f);
        float D = alpha2 / (3.14159265f * denom * denom + 1e-5f);

        float F0 = lerp(0.04f, 0.95f, metallic);
        float F = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
        float specular = (D * F) / (4.0f * max(NdotV, 0.05f) * max(NdotL, 0.05f) + 0.05f);

        litColor += (diffuse + specular * radiance) * radiance;
    }

    if ((u_LightCounts.z & 0x100u) != 0u && cascadeIndex < 0)
    {
        [unroll] for (int c = 0; c < 4; ++c)
        {
            float4 sp = mul(u_ShadowViewProjections[c], float4(input.v_WorldPosition, 1.0f));
            float3 pr = sp.xyz / max(sp.w, 1e-5f);
            float2 uv = pr.xy * 0.5f + 0.5f;
            if (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f
                && (((u_LightCounts.z & 1u) > 0u) ? (pr.z * 0.5f + 0.5f) : pr.z) >= 0.0f && (((u_LightCounts.z & 1u) > 0u) ? (pr.z * 0.5f + 0.5f) : pr.z) <= 1.0f)
            {
                cascadeIndex = c;
                break;
            }
        }
    }

    if ((u_LightCounts.z & 0x100u) != 0u && cascadeIndex >= 0)
    {
        float3 cascadeTints[4] = {
            float3(1.0f, 0.25f, 0.25f), // 0: 红色 (近景)
            float3(0.25f, 1.0f, 0.25f), // 1: 绿色
            float3(0.25f, 0.5f, 1.0f),  // 2: 蓝色
            float3(1.0f, 1.0f, 0.25f)   // 3: 黄色 (远景)
        };
        litColor *= cascadeTints[clamp(cascadeIndex, 0, 3)];
    }

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
    [[vk::location(0)]] float v_Hole : HOLE;
};

VS_SHADOW_OUTPUT VSShadowMain(VS_INPUT input)
{
    VS_SHADOW_OUTPUT output;
    float4 worldPos = mul(u_Model, float4(input.a_Position, 1.0f));
    uint cascade = (uint)clamp(u_TerrainParams.z, 0.0f, 3.0f);
    output.Position = mul(u_ShadowViewProjections[cascade], worldPos);
    output.v_Hole = input.a_Hole;
    return output;
}

void PSShadowMain(VS_SHADOW_OUTPUT input)
{
    if (u_SlopeParams.w > 0.5f && input.v_Hole > 0.5f)
        discard;
}
