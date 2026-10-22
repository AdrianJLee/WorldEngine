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

struct FoliageUniforms
{
    float4 WindParams;     // x = windSpeed, y = windStrength, z = time, w = trunkStiffness
    float4 WindDirection;  // xyz = windDir (normalized), w = unused
    float4 CullParams;     // x = maxCullDist, y = fadeRange, z = reserved, w = reserved
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
    for (uint i = 0; i < dirLightCount && i < 8; ++i)
    {
        float3 L = normalize(-u_Light.Lights[i].DirectionRange.xyz);
        
        // 实体碎石：真实单面朗伯漫反射光照 (Clamped Lambertian Diffuse)，背光面自然处于背阴处
        // 植被薄片：双面漫反射 (Two-sided diffuse) + 背光次表面透光 (Subsurface Translucency)
        float NdotL = isRock ? max(dot(N, L), 0.0f) : abs(dot(N, L));
        float backLight = isRock ? 0.0f : (max(dot(-N, L), 0.0f) * 0.35f);

        float3 lightColor = u_Light.Lights[i].ColorIntensity.rgb * u_Light.Lights[i].ColorIntensity.a;
        directLight += (NdotL * 0.85f + backLight) * lightColor * albedo.rgb * rootAO;
    }

    float3 finalColor = ambient + directLight;
    return float4(finalColor, albedo.a);
}

