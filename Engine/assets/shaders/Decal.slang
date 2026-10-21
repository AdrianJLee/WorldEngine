// Decal.slang - 3D Projected Decal Shader

struct CameraUniforms
{
    float4x4 ViewProjection;
    float4x4 View;
    float4x4 Projection;
    float4 CameraPosition;
};

struct DecalUniforms
{
    float4x4 DecalModel;
    float4x4 InvDecalModel;
    float4 DecalParams; // x = fadeFactor, y = normalCutoff, z = reserved, w = reserved
};

[[vk::binding(0, 0)]] ConstantBuffer<CameraUniforms> u_Camera;
[[vk::binding(1, 1)]] ConstantBuffer<DecalUniforms> u_Decal;
[[vk::binding(4, 2)]] Sampler2D u_DecalTexture;

struct VSInput
{
    [[vk::location(0)]] float3 Position : POSITION;
};

struct PSInput
{
    float4 Position      : SV_Position;
    float3 LocalPosition : POSITION0;
};

[shader("vertex")]
PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(u_Decal.DecalModel, float4(input.Position, 1.0f));
    output.Position = mul(u_Camera.ViewProjection, worldPos);
    output.LocalPosition = input.Position;
    return output;
}

[shader("fragment")]
float4 PSMain(PSInput input) : SV_Target
{
    // 立方体局部空间裁剪
    if (abs(input.LocalPosition.x) > 1.0f || abs(input.LocalPosition.y) > 1.0f || abs(input.LocalPosition.z) > 1.0f)
        discard;

    float2 uv = input.LocalPosition.xz * 0.5f + 0.5f;
    float4 decalColor = u_DecalTexture.Sample(uv);

    float depthFade = 1.0f - abs(input.LocalPosition.y);
    float alpha = decalColor.a * u_Decal.DecalParams.x * depthFade;

    if (alpha < 0.05f)
        discard;

    return float4(decalColor.rgb, alpha);
}
