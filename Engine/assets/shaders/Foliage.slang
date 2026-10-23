// Foliage.slang - WorldEngine 3D GPU Instanced Foliage & Vegetation Shader

struct CameraUniforms
{
    float4x4 ViewProjection;
    float4x4 View;
    float4x4 Projection;
    float4 CameraPosition;
};

struct Light
{
    float4 PositionType;
    float4 ColorIntensity;
    float4 DirectionRange;
    float4 SpotAngles;
};

struct LightUniforms
{
    float4x4 ShadowViewProjections[4];
    float4 CascadeSplits;
    float4 ShadowParams;
    float4 Ambient;
    uint4 LightCounts;
    Light Lights[8];
};

struct LocalWindSource
{
    float4 PositionRadius;     // xyz = world center, w = radius
    float4 DirectionStrength;  // xyz = direction / axis, w = strength
    float4 Params;             // x = speed, y = decay, z = type (0=Directional, 1=Radial, 2=Vortex), w = unused
};

struct FoliageUniforms
{
    float4 WindParams;     // x = windSpeed, y = windStrength, z = time, w = trunkStiffness
    float4 WindDirection;  // xyz = windDir (normalized), w = unused
    float4 CullParams;     // x = maxCullDist, y = fadeRange, z = localWindCount, w = reserved
    LocalWindSource LocalWinds[8];
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraUniforms> u_Camera;
[[vk::binding(2, 0)]] ConstantBuffer<LightUniforms> u_Light;
[[vk::binding(3, 0)]] Sampler2D u_ShadowMap;

[[vk::binding(1, 1)]] ConstantBuffer<FoliageUniforms> u_Foliage;

[[vk::binding(4, 2)]] Sampler2D u_AlbedoTexture;

struct VSInput
{
    [[vk::location(0)]] float3 Position : POSITION;
    [[vk::location(1)]] float3 Normal   : NORMAL;
    [[vk::location(2)]] float2 TexCoord : TEXCOORD0;

    // Per-instance attributes
    [[vk::location(4)]] float4 InstanceTransform0 : INSTANCE_XFORM0;
    [[vk::location(5)]] float4 InstanceTransform1 : INSTANCE_XFORM1;
    [[vk::location(6)]] float4 InstanceTransform2 : INSTANCE_XFORM2;
    [[vk::location(7)]] float4 InstanceTransform3 : INSTANCE_XFORM3;
    [[vk::location(8)]] float4 InstanceParams     : INSTANCE_PARAMS; // x=windPhase, y=ditherOffset, z=scale, w=typeIndex
};

struct PSInput
{
    float4 Position : SV_Position;
    float3 WorldPos : POSITION0;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD0;
    float4 Params   : TEXCOORD1; // x=distanceToCamera, y=ditherOffset, z=windPhase, w=typeIndex
};

static const float BayerMatrix4x4[16] = {
     0.0f / 16.0f,  8.0f / 16.0f,  2.0f / 16.0f, 10.0f / 16.0f,
    12.0f / 16.0f,  4.0f / 16.0f, 14.0f / 16.0f,  6.0f / 16.0f,
     3.0f / 16.0f, 11.0f / 16.0f,  1.0f / 16.0f,  9.0f / 16.0f,
    15.0f / 16.0f,  7.0f / 16.0f, 13.0f / 16.0f,  5.0f / 16.0f
};

[shader("vertex")]
PSInput VSMain(VSInput input)
{
    PSInput output;

    float3 localPos = input.Position;

    // 顶点风场扰动摇摆 (Wind Sway)
    float typeIdx = input.InstanceParams.w;
    bool isRock = (typeIdx > 2.5f && typeIdx < 3.5f);
    bool isBush = (typeIdx > 1.5f && typeIdx < 2.5f);

    float heightFactor = pow(clamp(localPos.y * 0.7f, 0.0f, 2.0f), 1.5f);
    if (isRock)
    {
        heightFactor = 0.0f; // 碎石坚硬无风场摇摆
    }
    else if (isBush)
    {
        heightFactor *= 0.35f; // 木质灌木具备更高抗弯刚度
    }
    float time = u_Foliage.WindParams.z;
    float speed = u_Foliage.WindParams.x;
    float strength = u_Foliage.WindParams.y;
    float phase = input.InstanceParams.x;

    float3 instancePos = input.InstanceTransform3.xyz;
    float spatialWave = dot(instancePos.xz, u_Foliage.WindDirection.xz) * 0.15f;
    float sway = sin(spatialWave + time * speed + phase) * strength * 0.35f;
    float microFlutter = cos(time * speed * 3.1f + phase * 2.0f) * 0.08f * strength;

    float3 windOffset = u_Foliage.WindDirection.xyz * (sway + microFlutter) * heightFactor;

    // 叠加局部多风源动力学 (Local Wind Sources Dynamics)
    int localWindCount = min(int(u_Foliage.CullParams.z + 0.5f), 8);
    for (int wi = 0; wi < localWindCount; ++wi)
    {
        float3 center = u_Foliage.LocalWinds[wi].PositionRadius.xyz;
        float radius = u_Foliage.LocalWinds[wi].PositionRadius.w;
        float3 toInstance = instancePos - center;
        float dist = length(toInstance);
        if (dist < radius && radius > 0.001f)
        {
            float normDist = dist / radius;
            float decay = max(0.1f, u_Foliage.LocalWinds[wi].Params.y);
            float falloff = pow(saturate(1.0f - normDist), decay);

            float localStrength = u_Foliage.LocalWinds[wi].DirectionStrength.w;
            float localSpeed = u_Foliage.LocalWinds[wi].Params.x;
            float windType = u_Foliage.LocalWinds[wi].Params.z;
            float3 axisDir = u_Foliage.LocalWinds[wi].DirectionStrength.xyz;

            float3 localDir = float3(0.0f, 0.0f, 0.0f);
            if (windType > 1.5f) // 2: Vortex (切向环形气旋)
            {
                float3 upAxis = (length(axisDir) > 0.1f) ? normalize(axisDir) : float3(0.0f, 1.0f, 0.0f);
                float3 radial = (dist > 0.001f) ? (toInstance / dist) : float3(1.0f, 0.0f, 0.0f);
                localDir = cross(upAxis, radial);
            }
            else if (windType > 0.5f) // 1: Radial / Impulse (中心向外辐射推挤)
            {
                localDir = (dist > 0.001f) ? (toInstance / dist) : float3(0.0f, 0.0f, 1.0f);
                localDir.y = max(localDir.y * 0.35f, -0.2f);
                localDir = normalize(localDir);
            }
            else // 0: DirectionalBox (局域定向风道)
            {
                localDir = (length(axisDir) > 0.1f) ? normalize(axisDir) : float3(1.0f, 0.0f, 0.0f);
            }

            float localWave = sin(time * localSpeed * 2.8f + normDist * 6.28f + phase) * 0.25f + 0.75f;
            float3 localDisplacement = localDir * (localStrength * falloff * localWave * heightFactor);
            windOffset += localDisplacement;
        }
    }

    localPos += windOffset;

    // 列向量精确线性组合，彻底杜绝转置/行优先歧义
    float3 worldPos = input.InstanceTransform0.xyz * localPos.x +
                      input.InstanceTransform1.xyz * localPos.y +
                      input.InstanceTransform2.xyz * localPos.z +
                      input.InstanceTransform3.xyz;
    float3 worldNormal = normalize(
        input.InstanceTransform0.xyz * input.Normal.x +
        input.InstanceTransform1.xyz * input.Normal.y +
        input.InstanceTransform2.xyz * input.Normal.z
    );

    // 根部法线微小抬高，彻底消除草丛根部与地表三角面重叠引起的 Z-fighting 闪烁 (实体碎石无需抬升)
    if (!isRock)
    {
        worldPos += worldNormal * 0.02f;
    }

    output.WorldPos = worldPos;
    output.Normal = worldNormal;
    output.TexCoord = input.TexCoord;
    output.Position = mul(u_Camera.ViewProjection, float4(worldPos, 1.0f));

    float dist = distance(worldPos, u_Camera.CameraPosition.xyz);
    output.Params = float4(dist, input.InstanceParams.y, input.InstanceParams.x, input.InstanceParams.w);

    return output;
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
        shadowPosition = mul(u_Light.ShadowViewProjections[c], float4(worldPosition, 1.0f));
        projected = shadowPosition.xyz / max(shadowPosition.w, 1e-5f);
        localUV = projected.xy * 0.5f + 0.5f;
        if (localUV.x >= 0.0f && localUV.x <= 1.0f && localUV.y >= 0.0f && localUV.y <= 1.0f
            && (((u_Light.LightCounts.z & 1u) > 0u) ? (projected.z * 0.5f + 0.5f) : projected.z) >= 0.0f && (((u_Light.LightCounts.z & 1u) > 0u) ? (projected.z * 0.5f + 0.5f) : projected.z) <= 1.0f)
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

    const float depth = (u_Light.LightCounts.z & 1u) > 0u ? (projected.z * 0.5f + 0.5f) : projected.z;
    const float cosTheta = saturate(dot(normal, toLightDirection));
    const float biasBase = max(u_Light.ShadowParams.y, 0.002f);
    const float bias = max(biasBase * (1.0f - cosTheta), biasBase * 0.25f);
    const float2 texel = (u_Light.ShadowParams.w / max(u_Light.ShadowParams.z, 1.0f)) * 0.5f;

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

[shader("fragment")]
float4 PSMain(PSInput input) : SV_Target
{
    // 视距 Screen-Door Dither 渐变消隐
    float maxDist = u_Foliage.CullParams.x;
    float fadeRange = max(u_Foliage.CullParams.y, 1.0f);
    float dist = input.Params.x;

    if (dist > maxDist)
        discard;

    if (dist > (maxDist - fadeRange))
    {
        float fadeAlpha = 1.0f - (dist - (maxDist - fadeRange)) / fadeRange;
        // input.Params.y 是实例固有的稳定随机数 [0..1]，随距离拉远平滑淡出，绝不随相机移动每帧在屏幕像素间跳动闪烁
        if (fadeAlpha < input.Params.y)
            discard;
    }

    float typeIdx = input.Params.w;
    bool isRock = (typeIdx > 2.5f && typeIdx < 3.5f);

    float4 albedo = u_AlbedoTexture.Sample(input.TexCoord);
    if (!isRock && albedo.a < 0.20f)
        discard;

    float3 N = normalize(input.Normal);
    
    // 环境光遮蔽: 碎石实体提供坚实地表环境光；植被提供叶片接触 AO
    float rootAO = isRock ? 0.88f : lerp(0.40f, 1.0f, saturate(1.0f - input.TexCoord.y * 0.85f));

    float3 ambient = u_Light.Ambient.rgb * u_Light.Ambient.a * albedo.rgb * rootAO;

    float3 directLight = float3(0.0f);
    uint dirLightCount = u_Light.LightCounts.x;
    int cascadeIndex = -1;
    for (uint i = 0; i < dirLightCount && i < 8; ++i)
    {
        float3 L = normalize(-u_Light.Lights[i].DirectionRange.xyz);
        
        // 实体碎石：真实单面朗伯漫反射光照 (Clamped Lambertian Diffuse)，背光面自然处于背阴处
        // 植被薄片：双面漫反射 (Two-sided diffuse) + 背光次表面透光 (Subsurface Translucency)
        float NdotL = isRock ? max(dot(N, L), 0.0f) : abs(dot(N, L));
        float backLight = isRock ? 0.0f : (max(dot(-N, L), 0.0f) * 0.35f);

        float visibility = 1.0f;
        if (u_Light.ShadowParams.x > 0.5f || (u_Light.LightCounts.z & 0x100u) != 0u)
        {
            int cIndex = -1;
            float shadowVis = SampleDirectionalShadow(input.WorldPos, N, L, cIndex);
            if (u_Light.ShadowParams.x > 0.5f)
                visibility = shadowVis;
            if (cIndex >= 0)
                cascadeIndex = cIndex;
        }

        float3 lightColor = u_Light.Lights[i].ColorIntensity.rgb * u_Light.Lights[i].ColorIntensity.a;
        directLight += (NdotL * 0.85f + backLight) * lightColor * albedo.rgb * rootAO * visibility;
    }

    if ((u_Light.LightCounts.z & 0x100u) != 0u && cascadeIndex < 0)
    {
        [unroll] for (int c = 0; c < 4; ++c)
        {
            float4 sp = mul(u_Light.ShadowViewProjections[c], float4(input.WorldPos, 1.0f));
            float3 pr = sp.xyz / max(sp.w, 1e-5f);
            float2 uv = pr.xy * 0.5f + 0.5f;
            if (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f
                && (((u_Light.LightCounts.z & 1u) > 0u) ? (pr.z * 0.5f + 0.5f) : pr.z) >= 0.0f && (((u_Light.LightCounts.z & 1u) > 0u) ? (pr.z * 0.5f + 0.5f) : pr.z) <= 1.0f)
            {
                cascadeIndex = c;
                break;
            }
        }
    }

    float3 finalColor = ambient + directLight;

    if ((u_Light.LightCounts.z & 0x100u) != 0u && cascadeIndex >= 0)
    {
        float3 cascadeTints[4] = {
            float3(1.0f, 0.25f, 0.25f), // 0: 红色 (近景)
            float3(0.25f, 1.0f, 0.25f), // 1: 绿色
            float3(0.25f, 0.5f, 1.0f),  // 2: 蓝色
            float3(1.0f, 1.0f, 0.25f)   // 3: 黄色 (远景)
        };
        finalColor *= cascadeTints[clamp(cascadeIndex, 0, 3)];
    }

    return float4(finalColor, albedo.a);
}

