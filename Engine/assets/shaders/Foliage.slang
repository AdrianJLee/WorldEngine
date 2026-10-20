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
};

struct LightUniforms
{
    float4x4 ShadowViewProjection;
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
    float4 Params   : TEXCOORD1; // x=distanceToCamera, y=ditherOffset, z=windPhase, w=unused
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

    float4x4 instanceModel = float4x4(
        input.InstanceTransform0,
        input.InstanceTransform1,
        input.InstanceTransform2,
        input.InstanceTransform3
    );

    float3 localPos = input.Position;

    // 顶点风场扰动摇摆 (Wind Sway)
    // 根部坚固锚定 (y <= 0 不动), 随着高度递增呈幂次弯曲
    float heightFactor = pow(clamp(localPos.y * 0.5f, 0.0f, 2.0f), 1.5f);
    float time = u_Foliage.WindParams.z;
    float speed = u_Foliage.WindParams.x;
    float strength = u_Foliage.WindParams.y;
    float phase = input.InstanceParams.x;

    float3 instancePos = float3(instanceModel[3].x, instanceModel[3].y, instanceModel[3].z);
    float spatialWave = dot(instancePos.xz, u_Foliage.WindDirection.xz) * 0.15f;
    float sway = sin(spatialWave + time * speed + phase) * strength;
    float microFlutter = cos(time * speed * 3.1f + phase * 2.0f) * 0.2f * strength;

    float3 windOffset = u_Foliage.WindDirection.xyz * (sway + microFlutter) * heightFactor;
    localPos += windOffset;

    float4 worldPos = mul(instanceModel, float4(localPos, 1.0f));
    output.WorldPos = worldPos.xyz;

    float3x3 normalMatrix = float3x3(instanceModel[0].xyz, instanceModel[1].xyz, instanceModel[2].xyz);
    output.Normal = normalize(mul(normalMatrix, input.Normal));
    output.TexCoord = input.TexCoord;

    output.Position = mul(u_Camera.ViewProjection, worldPos);

    float dist = distance(worldPos.xyz, u_Camera.CameraPosition.xyz);
    output.Params = float4(dist, input.InstanceParams.y, input.InstanceParams.x, 0.0f);

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
        int x = int(input.Position.x) % 4;
        int y = int(input.Position.y) % 4;
        float threshold = BayerMatrix4x4[y * 4 + x];
        if (fadeAlpha < threshold)
            discard;
    }

    float4 albedo = u_AlbedoTexture.Sample(input.TexCoord);
    if (albedo.a < 0.33f)
        discard;

    float3 N = normalize(input.Normal);
    float3 ambient = u_Light.Ambient.rgb * u_Light.Ambient.a * albedo.rgb;

    float3 directLight = float3(0.0f);
    uint dirLightCount = u_Light.LightCounts.x;
    for (uint i = 0; i < dirLightCount && i < 8; ++i)
    {
        float3 L = normalize(-u_Light.Lights[i].DirectionRange.xyz);
        float NdotL = max(dot(N, L), 0.0f);

        // 叶片背光透光次表面半透散射 (Subsurface Translucency)
        float backLight = max(dot(-N, L), 0.0f) * 0.4f;

        float3 lightColor = u_Light.Lights[i].ColorIntensity.rgb * u_Light.Lights[i].ColorIntensity.a;
        directLight += (NdotL + backLight) * lightColor * albedo.rgb;
    }

    float3 finalColor = ambient + directLight;
    return float4(finalColor, albedo.a);
}
